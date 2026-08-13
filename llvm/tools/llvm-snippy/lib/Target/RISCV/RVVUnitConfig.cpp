//===-- RVVUnitConfig.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVVUnitConfig.h"
#include "RISCVGenerated.h"
#include "TargetConfig.h"

#include "snippy/Config/Valuegram.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLProbableItems.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include "RISCVSubtarget.h"

#include <cmath>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#define DEBUG_TYPE "snippy-rvv-config"
#define kProbabilityThreshold 0.001

// TODO:
// * [WIP] implement proper construction of legal VM
// * implement a test to check that state dump for non-simplified test works
// * implement a test that masks-out most vector registers and checks for the
//   final state
namespace llvm {
namespace snippy {

extern cl::OptionCategory SnippyRISCVOptions;

static snippy::opt<bool> NoReservedCfgRVV(
    "riscv-disable-reserved-sew-lmul",
    cl::desc(
        "This option disables the generation of reserved SEW and LMUL values"),
    cl::Hidden, cl::init(false), cl::cat(SnippyRISCVOptions));

static snippy::opt<std::string> DumpDiscardedRVVConfigurations(
    "riscv-dump-discarded-rvv-configurations",
    cl::desc("Print information about discarded due to incompatibility "
             "riscv-vector-unit configurations (VL, VM and RVV "
             "configuration generators"),
    cl::Hidden, cl::init(""), cl::ValueOptional, cl::cat(SnippyRISCVOptions));

} // namespace snippy
} // namespace llvm

namespace {

using namespace llvm;
using namespace llvm::snippy;

template <typename ItType>
static bool checkWeightsNonNegative(ItType Begin, ItType End) {
  return std::all_of(Begin, End, [](const auto Item) { return Item >= 0; });
}

template <typename ItType>
static bool checkNonZeroWeightPresent(ItType Begin, ItType End) {
  return std::any_of(Begin, End, [](const auto Item) { return Item > 0.0; });
}

template <typename ItType>
std::string checkWeights(ItType Begin, ItType End, const llvm::Twine &What) {
  using value_type = typename std::iterator_traits<ItType>::value_type;
  static_assert(std::is_convertible_v<value_type, double>,
                "Element type must be convertible to double");

  if (!checkWeightsNonNegative(Begin, End))
    return (What + ": weights must be non-negative!").str();

  if (!checkNonZeroWeightPresent(Begin, End))
    return (What + ": at least one weight must be positive!").str();
  return "";
}

template <typename ItType>
std::string checkWeights(llvm::iterator_range<ItType> Range,
                         const llvm::Twine &What) {
  return checkWeights(Range.begin(), Range.end(), What);
}

struct ModeChangeP final {
  // Indicates that ModeChangeP comes from the histogram and
  // not from the mode-change-bias::P
  struct ProbIsDeduced final {
    static constexpr StringLiteral Str = "deduced";
  };

  ModeChangeP() = default;
  ModeChangeP(double P) : Value(P) {}
  ModeChangeP(ProbIsDeduced) : Value(ProbIsDeduced()) {}

  bool isDeduced() const {
    return std::holds_alternative<ProbIsDeduced>(Value);
  }

  bool isNumerical() const { return std::holds_alternative<double>(Value); }

  double getAsDouble() const {
    assert(isNumerical());
    return std::get<double>(Value);
  }

private:
  std::variant<ProbIsDeduced, double> Value;
};

struct ModeChangeBias final {
  // Probability of generating a support mode-changing instruction
  // after a primary instruction
  ModeChangeP ModeChangeProb = ModeChangeP::ProbIsDeduced();

  // Probability of choosing an illegal configuration when a mode-changing
  // instruction is selected. Illegal configuration occurs when the {SEW, LMUL}
  // pair violates the target's constraints based on VLEN and ELEN.
  double SetVillP = 0.0;
};

struct RVVConfigurationSpace {
  ModeChangeBias Guides;
  RVVUnitInfo VUInfo;

  static constexpr auto kUnitName = "riscv-vector-unit";
  static void mapYaml(llvm::yaml::IO &IO,
                      std::optional<RVVConfigurationSpace> &CS);
};

struct VectorUnitRules {
  RVVConfigurationSpace Config;
};

template <typename T> struct ConfigurationElement {
  using UnderlyingType = std::underlying_type_t<T>;
  double P;
  UnderlyingType Value;
};

ModeChangeInfo deriveModeSwitchingProbability(const Config &Cfg,
                                              const ModeChangeBias &Bias) {
  const auto &Hist = Cfg.Histogram;
  double TotalWeight = Hist.getTotalWeight();
  ModeChangeInfo Result;
  Result.ProbSetVill = Bias.SetVillP;
  Result.TotalHistWeight = TotalWeight;

  bool RVVPresentInHistogram = Hist.getOpcodesProbability([](unsigned Opcode) {
    return isRVV(Opcode);
  }) > 0.0;
  Result.RVVPresentInHistogram = RVVPresentInHistogram;

  // If no RVV instructions are found in the histogram we don't generate any
  // additional VSETs, even if there is mode-change-bias specified.
  // (VSETs can still appear in register initialization though)
  if (!RVVPresentInHistogram) {
    Result.VSETPresentInHistogram = false;
    return Result;
  }

  bool VSETPresentInHistogram = Hist.getOpcodesProbability([](unsigned Opcode) {
    return isRVVModeSwitch(Opcode);
  }) > 0.0;
  Result.VSETPresentInHistogram = VSETPresentInHistogram;

  if (Bias.ModeChangeProb.isDeduced() && !VSETPresentInHistogram)
    snippy::fatal(
        "No VSET instruction detected in histogram. With RVV you must specify "
        "mode-change-bias P or mode-changing instructions in histogram");

  if (!Bias.ModeChangeProb.isDeduced() && VSETPresentInHistogram)
    snippy::fatal(
        Twine(
            "It is forbidden to specify any mode-change-bias P other than \"") +
        ModeChangeP::ProbIsDeduced::Str +
        "\" when VSET* instructions are present in histogram");

  assert(Bias.ModeChangeProb.isDeduced() == VSETPresentInHistogram);

  if (!VSETPresentInHistogram) {
    double ModeChangeBiasP = Bias.ModeChangeProb.getAsDouble();
    // FIXME: Currently there is a quirky behavior that we keep for
    // backward compatibility reasons:
    // if [VSET* instructions are not found in the histogram] &&
    //    [riscv-vector-unit::mode-change-bias not specified] then
    //    there will be an error
    // but if [VSET* instructions are not found in the histogram] &&
    //    [riscv-vector-unit::mode-change-bias::P = 0] then
    //    we must generate 1 VSET* per MBB
    //
    // This workaround is not perfect, as if there will be more than 1/epsilon
    // instructions requested, we might end up with more than 1 VSET* per MBB.
    if (ModeChangeBiasP < std::numeric_limits<double>::epsilon())
      ModeChangeBiasP = std::numeric_limits<double>::epsilon();

    Result.WeightVSETVL = TotalWeight * ModeChangeBiasP / 3.0;
    Result.WeightVSETVLI = TotalWeight * ModeChangeBiasP / 3.0;
    Result.WeightVSETIVLI = TotalWeight * ModeChangeBiasP / 3.0;
    return Result;
  }

  Result.WeightVSETVL = Hist.weight(RISCV::VSETVL);
  Result.WeightVSETVLI = Hist.weight(RISCV::VSETVLI);
  Result.WeightVSETIVLI = Hist.weight(RISCV::VSETIVLI);
  return Result;
}

// Declares static methods which are used by the class factory.
//
// static tryBuildFromString() can return 3 things:
//  - unique_ptr: a valid pointer - success
//  - nullptr: no match - should try next generator
//  - llvm::Error: match did happen, but some limitation was hit
#define DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(InterfaceName,             \
                                                    DerivedName, ID)           \
  static constexpr const char *kID = ID;                                       \
  std::string identify() const override { return ID; }                         \
                                                                               \
  static std::string getErrorName() { return std::string("'") + kID + "'"; }   \
                                                                               \
  static Expected<std::unique_ptr<InterfaceName>> tryBuildFromString(          \
      StringRef S) {                                                           \
    if (S != kID)                                                              \
      return nullptr;                                                          \
    return std::make_unique<DerivedName>();                                    \
  }

struct MaxPossibleVLGen final : VLGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VLGeneratorInterface,
                                              MaxPossibleVLGen, "max_encodable")

  VLDistributionType getDistribution(unsigned VLMax) const override {
    // VL can be [0, VLMax], so we need VLMax + 1 values
    VLDistributionType Result(VLMax + 1);
    Result.back() = 1.0;
    return Result;
  }
};

// It's different from MaxPossibleVLGen in that, in case when the vlmax value is
// greater than kMaxVLForVSETIVLI, it will not be generated. In case of the
// MaxPossibleVLGen, the value will be generated and reduced.
struct MaxVLGenerator final : VLGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VLGeneratorInterface,
                                              MaxVLGenerator, "vlmax")

  // Exactly the same as MaxPossibleVLGen
  VLDistributionType getDistribution(unsigned VLMax) const override {
    VLDistributionType Result(VLMax + 1);
    Result.back() = 1.0;
    return Result;
  }

  // Unapplicable (all weights are zero) if VLMax > kMaxVLForVSETIVLI
  VLDistributionType getDistributionForVSETIVLI(unsigned VLMax) const override {
    VLDistributionType Result(kMaxVLForVSETIVLI + 1);
    if (VLMax <= kMaxVLForVSETIVLI)
      Result[VLMax] = 1.0;
    return Result;
  }
};

struct LegalVLGenerator final : VLGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VLGeneratorInterface,
                                              LegalVLGenerator, "any_legal")

  VLDistributionType getDistribution(unsigned VLMax) const override {
    return VLDistributionType(VLMax + 1, 1.0 / (VLMax + 1));
  }
};

struct LegalVLNonZeroGenerator final : VLGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VLGeneratorInterface,
                                              LegalVLNonZeroGenerator,
                                              "any_legal_non_zero")

  VLDistributionType getDistribution(unsigned VLMax) const override {
    VLDistributionType Result(VLMax + 1);
    for (auto &W : drop_begin(Result))
      W = 1.0 / VLMax;
    return Result;
  }
};

struct UnmaskedVMGenerator final : VMGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VMGeneratorInterface,
                                              UnmaskedVMGenerator, "all_ones")

  unsigned getMinRequiredVL() const override { return 0; }

  APInt generate(unsigned VL) const override { return APInt::getAllOnes(VL); }
};

struct LegalVMGenerator final : VMGeneratorInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(VMGeneratorInterface,
                                              LegalVMGenerator, "any_legal")

  unsigned getMinRequiredVL() const override { return 0; }

  APInt generate(unsigned VL) const override {
    auto MaxValue = APInt::getAllOnes(VL);
    return APInt(RandEngine::genInRangeInclusive(MaxValue));
  }
};

struct ImmVLGen : public VLGeneratorInterface {
  ImmVLGen(unsigned Value, std::string Context)
      : Value(Value), Context(std::move(Context)) {}

  static constexpr const char *kID = "imm";
  std::string identify() const override {
    return std::string(kID) + "_" + Context;
  }

  static std::string getErrorName() { return "Non-negative integer"; }

  static Expected<std::unique_ptr<VLGeneratorInterface>>
  tryBuildFromString(StringRef S) {
    Expected<FormattedAPIntWithSign> ExpValue =
        FormattedAPIntWithSign::fromString(S);
    if (auto Err = ExpValue.takeError()) {
      // Simply means that we can't build this generator
      consumeError(std::move(Err));
      return nullptr;
    }
    if (ExpValue->Number.IsSigned)
      return nullptr;

    auto APIntVal = ExpValue->Number.Value;
    if (APIntVal.getActiveBits() > sizeof(Value) * CHAR_BIT)
      return makeFailure(Errc::InvalidArgument,
                         formatv("VLs can't be bigger than {0}",
                                 std::numeric_limits<unsigned>::max()));

    return std::make_unique<ImmVLGen>(APIntVal.getZExtValue(), S.str());
  }

  VLDistributionType getDistribution(unsigned VLMax) const override {
    VLDistributionType Result(VLMax + 1);
    if (Value <= VLMax)
      Result[Value] = 1.0;
    return Result;
  }

private:
  unsigned Value;
  std::string Context;
};

struct ImmVMGen : public VMGeneratorInterface {
  ImmVMGen(APInt Value, std::string Context)
      : Value(std::move(Value)), Context(std::move(Context)) {}

  static constexpr const char *kID = "imm";
  std::string identify() const override {
    return std::string(kID) + "_" + Context;
  }

  static std::string getErrorName() { return "Non-negative integer"; }

  static Expected<std::unique_ptr<VMGeneratorInterface>>
  tryBuildFromString(StringRef S) {
    Expected<FormattedAPIntWithSign> ExpValue =
        FormattedAPIntWithSign::fromString(S);
    if (auto Err = ExpValue.takeError()) {
      // Simply means that we can't build this generator
      consumeError(std::move(Err));
      return nullptr;
    }
    if (ExpValue->Number.IsSigned)
      return nullptr;

    return std::make_unique<ImmVMGen>(ExpValue->Number.Value, S.str());
  }

  unsigned getMinRequiredVL() const override { return Value.getActiveBits(); }

  APInt generate(unsigned VL) const override {
    assert((getMinRequiredVL() <= VL) &&
           "Generation request should be made only for valid VMs");
    return Value;
  }

private:
  APInt Value;
  std::string Context;
};

template <typename BaseType>
static Expected<std::unique_ptr<BaseType>> tryConstructFromString(StringRef S) {
  // None of the derived types matched the string
  return nullptr;
}

template <typename BaseType, typename T, typename... U>
static Expected<std::unique_ptr<BaseType>> tryConstructFromString(StringRef S) {
  Expected<std::unique_ptr<BaseType>> ObjOrErr = T::tryBuildFromString(S);
  // Error means there was a match, but some limitation was hit - report it
  if (!ObjOrErr)
    return makeFailure(Errc::InvalidConfiguration,
                       formatv("{0} was matched, but: {1}", T::getErrorName(),
                               toString(ObjOrErr.takeError())));
  // Successful match
  if (*ObjOrErr != nullptr)
    return ObjOrErr;
  // No match, try next
  return tryConstructFromString<BaseType, U...>(S);
}

template <typename... Types> std::string getAllErrorNames() {
  std::string Result;
  // Fold expression over all types
  ((Result += Types::getErrorName() + ", "), ...);
  assert(!Result.empty());
  Result.pop_back(); // remove last comma
  Result.pop_back(); // remove space
  return Result;
}

template <typename T>
constexpr bool has_error_name_method_v =
    std::is_invocable_r_v<std::string, decltype(&T::getErrorName)>;

template <typename BaseT, typename T>
constexpr bool has_try_build_from_string_v =
    std::is_invocable_r_v<Expected<std::unique_ptr<BaseT>>,
                          decltype(&T::tryBuildFromString), StringRef>;

// Try to construct object of type BaseType from string S.
// If none of the derived types can be constructed, return
// error with string containing 'names' of all derived types.
//
// static tryBuildFromString() can return 3 things:
//  - unique_ptr: a valid pointer - success
//  - nullptr: no match - should try next generator
//  - llvm::Error: match did happen, but some limitation was hit
template <typename BaseType, typename... Types>
Expected<std::unique_ptr<BaseType>> constructFromString(StringRef S) {
  static_assert((has_error_name_method_v<Types> && ...),
                "All types must have static std::string getErrorName()");

  static_assert(
      (has_try_build_from_string_v<BaseType, Types> && ...),
      "All types must have static Expected<std::unique_ptr<BaseType>> "
      "tryBuildFromString(StringRef)");

  auto ObjOrErr = tryConstructFromString<BaseType, Types...>(S);

  // Some type matched, but some limitation was hit
  if (!ObjOrErr)
    return ObjOrErr;
  // Successful match
  if (*ObjOrErr != nullptr)
    return ObjOrErr;
  return makeFailure(
      Errc::InvalidArgument,
      formatv("'{0}' is none of: {1}", S.str(), getAllErrorNames<Types...>()));
}

template <typename Result> struct GeneratorFactory;

template <> struct GeneratorFactory<VLGeneratorHolder> {
  static Expected<VLGeneratorHolder> createOrErr(StringRef ID) {
    return constructFromString<VLGeneratorInterface, ImmVLGen, MaxPossibleVLGen,
                               MaxVLGenerator, LegalVLGenerator,
                               LegalVLNonZeroGenerator>(ID);
  }

  static VLGeneratorHolder create(StringRef ID) {
    return cantFail(createOrErr(ID));
  }
};
template <> struct GeneratorFactory<VMGeneratorHolder> {
  static Expected<VMGeneratorHolder> createOrErr(StringRef ID) {
    return constructFromString<VMGeneratorInterface, ImmVMGen,
                               UnmaskedVMGenerator, LegalVMGenerator>(ID);
  }

  static VMGeneratorHolder create(StringRef ID) {
    return cantFail(createOrErr(ID));
  }
};
} // namespace

namespace llvm {
namespace snippy {

static std::string toString(VSEW SEW) {
  switch (SEW) {
  case VSEW::SEWReserved1:
    return "eReserved1";
  case VSEW::SEWReserved2:
    return "eReserved2";
  case VSEW::SEWReserved3:
    return "eReserved3";
  case VSEW::SEWReserved4:
    return "eReserved4";
  case VSEW::SEW8:
  case VSEW::SEW16:
  case VSEW::SEW32:
  case VSEW::SEW64:
    return 'e' + std::to_string(static_cast<unsigned>(SEW));
  }
  llvm_unreachable("Unknown SEW");
}

static std::string toString(VLMUL LMUL) {
  switch (LMUL) {
  case VLMUL::LMUL_RESERVED:
    return "mReserved";
  case VLMUL::LMUL_1:
    return "m1";
  case VLMUL::LMUL_2:
    return "m2";
  case VLMUL::LMUL_4:
    return "m4";
  case VLMUL::LMUL_8:
    return "m8";
  case VLMUL::LMUL_F2:
    return "mf2";
  case VLMUL::LMUL_F4:
    return "mf4";
  case VLMUL::LMUL_F8:
    return "mf8";
  }
  llvm_unreachable("Unknown LMUL");
}

static std::string toString(VTAMode TA) {
  switch (TA) {
  case VTAMode::TA:
    return "ta";
  case VTAMode::TU:
    return "tu";
  }
  llvm_unreachable("Unknown TA");
}

static std::string toString(VMAMode MA) {
  switch (MA) {
  case VMAMode::MA:
    return "ma";
  case VMAMode::MU:
    return "mu";
  }
  llvm_unreachable("Unknown MA");
}

static std::string toString(VXRMMode XRM) {
  switch (XRM) {
  case VXRMMode::RNE:
    return "rne";
  case VXRMMode::RNU:
    return "rnu";
  case VXRMMode::RDN:
    return "rdn";
  case VXRMMode::RON:
    return "ron";
  }
  llvm_unreachable("Unknown XRM");
}

static void printRawSewLmulProbs(raw_ostream &OS, const SEWInfo &SEWProbs,
                                 const LMULInfo &LMULProbs) {
  OS << "=== Raw SEW Probabilities ===\n";
  SEWProbs.print(OS, [](VSEW SEW) { return toString(SEW); });
  OS << "=== Raw LMUL Probabilities ===\n";
  LMULProbs.print(OS, [](VLMUL LMUL) { return toString(LMUL); });
}

static void printModeList(raw_ostream &OS, const SewLmulDistribution &Dist) {
  OS << "=== Raw mode-list Probabilities ===\n";
  Dist.print(OS, [](const SewLmulPair &SewLmul) {
    auto [SEW, LMUL] = SewLmul;
    return formatv("[{0}, {1}]", toString(SEW), toString(LMUL));
  });
}

static void printCombinedSewLmulProbs(raw_ostream &OS,
                                      const SewLmulDistribution &Dist) {
  constexpr auto SewSize = SEWInfo::size();
  constexpr auto LmulSize = LMULInfo::size();
  double Probs[SewSize][LmulSize] = {};

  for (auto &[SewLmul, P] : Dist) {
    auto [SEW, LMUL] = SewLmul;
    unsigned SEWIdx = SEWInfo::Mapping::toIdx(SEW);
    unsigned LMULIdx = LMULInfo::Mapping::toIdx(LMUL);
    Probs[SEWIdx][LMULIdx] = P;
  }

  // Print a table like:
  //             | mReserved | mf8 | mf4 | mf2 |  m1 |  m2 |  m4 |  m8
  //          e8 |       0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  //         e16 |       0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  //     . . . .
  //  eReserved3 |       0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  //  eReserved4 |       0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5

  constexpr std::array ColumnsWidths = {10, 5, 5, 5, 5, 5, 5, 5};
  constexpr std::array LMULs = {
      VLMUL::LMUL_RESERVED, VLMUL::LMUL_F8, VLMUL::LMUL_F4, VLMUL::LMUL_F2,
      VLMUL::LMUL_1,        VLMUL::LMUL_2,  VLMUL::LMUL_4,  VLMUL::LMUL_8};
  constexpr auto SewWidth = 10;

  OS << right_justify("", SewWidth);
  for (const auto &[LMUL, ColWidth] : zip_equal(LMULs, ColumnsWidths))
    OS << " |" << right_justify(toString(LMUL), ColWidth);
  OS << "\n";

  for (auto SEW : SEWEnumList::Arr) {
    OS << right_justify(toString(SEW), SewWidth);
    for (const auto &[LMUL, ColWidth] : zip_equal(LMULs, ColumnsWidths)) {
      double Prob =
          Probs[SEWInfo::Mapping::toIdx(SEW)][LMULInfo::Mapping::toIdx(LMUL)];
      OS << " |" << format("%*.2f", ColWidth, Prob * 100);
    }
    OS << "\n";
  }
  OS << "\n";
}

[[maybe_unused]] static void printVLProbs(raw_ostream &OS,
                                          const VLDistributionType &Dist) {
  ProbableItems<size_t> VLProbs;
  for (const auto &[VL, P] : enumerate(Dist))
    VLProbs.push_back({VL, P});
  VLProbs.print(OS);
}

// This doesn't account for VL. There might be a {SEW, LMUL} pair with
// no legal VLs from config for it.
static SewLmulDistribution
buildRawSewLmulDistribution(unsigned ELEN, unsigned VLEN,
                            const RVVUnitInfo &VUInfo, double PVill) {
  // All values, including the reserved ones
  SEWInfo AllSewWeights = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  LMULInfo AllLMULWeights = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

  // Zero-out the reserved ones if required
  if (NoReservedCfgRVV) {
    for (auto &[SEW, Prob] : AllSewWeights) {
      if (!isLegalSEW(SEW))
        Prob = 0.0;
    }
    for (auto &[LMUL, Prob] : AllLMULWeights) {
      if (!isLegalLMUL(LMUL))
        Prob = 0.0;
    }
  }

  SewLmulDistribution Dist =
      jointProbabilityDistribution(AllSewWeights, AllLMULWeights);

  auto IsLegal = [&](const ProbableElement<SewLmulPair> &Item) -> bool {
    auto [SEW, LMUL] = Item.Element;
    return isLegalSewLmul(ELEN, VLEN, SEW, LMUL);
  };
  auto LegalRange = make_filter_range(Dist, IsLegal);
  auto IllegalRange = make_filter_range(Dist, std::not_fn(IsLegal));

  // Now fill the distribution with values from the config
  //
  // Note that at the moment users can't set probabilities for reserved
  // values of SEW and LMUL, so we copy only probs for legal values
  // and ignore the rest (IllegalRange). All illegal configs will have the same
  // probability according to PVill.

  if (!VUInfo.ModeList.empty()) {
    // We're given an exact list of configs. Use it
    auto ModeListProbs = VUInfo.ModeList;
    ModeListProbs.normalizeProbs();
    LLVM_DEBUG(printModeList(dbgs(), ModeListProbs));

    // Zero-out all configs, then add probs to ones that were requested
    for (auto &[SewLmul, P] : LegalRange)
      P = 0.0;
    for (const auto &[ModeListEntry, ModeListP] : ModeListProbs) {
      auto [SEW, LMUL] = ModeListEntry;
      if (!isLegalSewLmul(ELEN, VLEN, SEW, LMUL)) {
        // FIXME: We parse SEW as `sew_8`, but we print it as `e8`.
        // This is the reason for the erase here. At some point we
        // should switch to using `e8` everywhere.
        snippy::warn(
            WarningName::InconsistentOptions,
            formatv("RVV mode-list entry [sew_{0}, {1}] is illegal "
                    "for the target and will be ignored",
                    /*erasing first letter 'e'*/ toString(SEW).erase(0, 1),
                    toString(LMUL)),
            formatv("ELEN={0}, VLEN={1}", ELEN, VLEN));
        continue;
      }

      auto EntryIt = find_if(
          LegalRange,
          [&ModeListE = ModeListEntry](const ProbableElement<SewLmulPair> &V) {
            return V.Element == ModeListE;
          });
      assert(EntryIt != LegalRange.end());
      EntryIt->Prob += ModeListP;
    }
  } else {
    // Build from marginal distributions of SEW and LMUL
    assert(VUInfo.VTYPE.SEW.has_value() && VUInfo.VTYPE.LMUL.has_value());
    auto SEWProbs = normalizeWeights(*VUInfo.VTYPE.SEW);
    auto LMULProbs = normalizeWeights(*VUInfo.VTYPE.LMUL);
    LLVM_DEBUG(printRawSewLmulProbs(dbgs(), SEWProbs, LMULProbs));

    for (auto &[SewLmul, P] : LegalRange) {
      auto [SEW, LMUL] = SewLmul;
      P = SEWProbs[SEW] * LMULProbs[LMUL];
    }
  }

  // The total probability of all illegal combinations must be PVill. The
  // total probability of all legal combinations must be 1 - PVill.
  auto AddProb = [](double Acc, const ProbableElement<SewLmulPair> &E) {
    return Acc + E.Prob;
  };
  double TotalLegalWeight =
      std::accumulate(LegalRange.begin(), LegalRange.end(), 0.0, AddProb);
  double TotalIllegalWeight =
      std::accumulate(IllegalRange.begin(), IllegalRange.end(), 0.0, AddProb);

  if (isZero(TotalLegalWeight) && !isZero(1.0 - PVill))
    fatal(
        "RVV Config: no legal configuration detected and Pvill != 1, aborting");
  // Should never happen as there're always reserved SEW and LMUL values
  // (which are always illegal)
  if (isZero(TotalIllegalWeight) && !isZero(PVill))
    fatal("RVV Config: no illegal configuration detected and Pvill != 0, "
          "aborting");

  for (auto &[SewLmul, P] : LegalRange)
    P *= (1.0 - PVill);
  for (auto &[SewLmul, P] : IllegalRange)
    P *= PVill;

  if (!isZero(TotalLegalWeight)) {
    for (auto &[SewLmul, P] : LegalRange)
      P /= TotalLegalWeight;
  }
  if (!isZero(TotalIllegalWeight)) {
    for (auto &[SewLmul, P] : IllegalRange)
      P /= TotalIllegalWeight;
  }
  assert(Dist.checkSumOfProbabilities());
  erase_if(Dist, [&](const auto &E) { return isZero(E.Prob); });

  LLVM_DEBUG(
      dbgs() << "=== {SEW, LMUL} Probabilities Accounting for Pvill (%) ===\n");
  LLVM_DEBUG(printCombinedSewLmulProbs(dbgs(), Dist));

  return Dist;
}

static void addToVLDistribution(unsigned MaxVL, double Weight,
                                const VLGeneratorHolder &VLGen,
                                VLDistributionType &ResultingDistribution,
                                bool IsForVSETIVLI) {
  assert(ResultingDistribution.size() > 0);
  // At the end of function the total sum of ResultingDistribution must either
  // stay the same or increase by Weight.

  auto VLDist = IsForVSETIVLI ? VLGen->getDistributionForVSETIVLI(MaxVL)
                              : VLGen->getDistribution(MaxVL);
  assert(VLDist.size() == ResultingDistribution.size());

  [[maybe_unused]] double TotalWeight =
      std::accumulate(VLDist.begin(), VLDist.end(), 0.0);
  assert(isZero(TotalWeight) || isZero(TotalWeight - 1.0, /*Tolerance=*/1e-6));

  for (auto [ResW, W] : zip_equal(ResultingDistribution, VLDist))
    ResW += W * Weight;
}

static VLDistributionType
buildDistributionForMaxVL(unsigned MaxVL,
                          const ProbableItems<VLGeneratorHolder> &VLGens,
                          bool IsForVSETIVLI) {
  assert(VLGens.size() != 0);

  unsigned DistributionSize = IsForVSETIVLI ? kMaxVLForVSETIVLI + 1 : MaxVL + 1;
  VLDistributionType ResultingDistribution(DistributionSize, 0.0);
  for (const auto &[VLGen, Weight] : VLGens)
    addToVLDistribution(MaxVL, Weight, VLGen, ResultingDistribution,
                        IsForVSETIVLI);

  // Can be all zeros (meaning that all VLGens are incompatible with the given
  // MaxVL), but otherwise should be normalized.
  if (!all_of(ResultingDistribution, [](double W) { return isZero(W); }))
    normalizeValues(ResultingDistribution);

  return ResultingDistribution;
}

static VLDistributionType buildDistributionForIllegalConfig(
    unsigned ELEN, unsigned VLEN,
    const ProbableItems<VLGeneratorHolder> &VLGens, bool IsForVSETIVLI) {
  // Treating illegal configurations as if they have
  // MaxVL == MaxPossibleVL(VLEN).
  return buildDistributionForMaxVL(getMaxPossibleVL(ELEN, VLEN), VLGens,
                                   IsForVSETIVLI);
}

} // namespace snippy
} // namespace llvm

namespace {
// Exists to cache and give out a VL distribution for each {SEW, LMUL} pair
class VlDistributionStorage final {
  using VLType = unsigned;

  const unsigned ELEN;
  const unsigned VLEN;
  // {MaxVL -> VLProbabilityDistribution}
  std::map<VLType, VLDistributionType> Distributions;

  // Tipically MaxVls are [0, 1, 2, 4, ... , VLEN].
  std::vector<VLType> getAllMaxVLs() {
    std::vector<VLType> MaxVls;
    for (const auto &SEW : SEWEnumList::Arr) {
      for (const auto &LMUL : LMULEnumList::Arr) {
        VLType MaxVL = computeVLMax(ELEN, VLEN, SEW, LMUL);
        MaxVls.push_back(MaxVL);
      }
    }
    std::sort(MaxVls.begin(), MaxVls.end());
    MaxVls.erase(llvm::unique(MaxVls), MaxVls.end());
    return MaxVls;
  }

public:
  VlDistributionStorage(unsigned ELEN, unsigned VLEN,
                        const ProbableItems<VLGeneratorHolder> &VLGens,
                        bool IsForVSETIVLI)
      : ELEN(ELEN), VLEN(VLEN) {
    assert(VLEN > 0);

    const auto &MaxVls = getAllMaxVLs();
    for (unsigned MaxVL : MaxVls) {
      // Illegal configurations have MaxVL == 0
      if (MaxVL == 0) {
        Distributions[MaxVL] = buildDistributionForIllegalConfig(
            ELEN, VLEN, VLGens, IsForVSETIVLI);
        continue;
      }
      Distributions[MaxVL] =
          buildDistributionForMaxVL(MaxVL, VLGens, IsForVSETIVLI);
    }
  }

  const VLDistributionType &get(VSEW SEW, VLMUL LMUL) const {
    VLType MaxVL = computeVLMax(ELEN, VLEN, SEW, LMUL);
    assert(Distributions.find(MaxVL) != Distributions.end());
    return Distributions.at(MaxVL);
  }
};

// Exists to cache and give out a {SEW, LMUL} distribution for each VL
class SewLmulDistributionStorage final {
  using VLType = unsigned;

  // {VL -> SewLmulProbabilityDistribution}
  std::map<VLType, SewLmulDistribution> Distributions;

public:
  SewLmulDistributionStorage(unsigned ELEN, unsigned VLEN,
                             const SewLmulDistribution &SewLmulDist) {
    const auto &VLSize = getMaxPossibleVL(ELEN, VLEN);
    // For each VL add only those {SEW, LMUL} that have VLMax >= VL
    for (unsigned VL = 0; VL <= VLSize; VL++) {
      SewLmulDistribution Dist;
      copy_if(SewLmulDist, std::back_inserter(Dist),
              [&](const ProbableElement<SewLmulPair> &Item) {
                auto [SEW, LMUL] = Item.Element;
                // If {SEW, LMUL} is illegal, we treat it as if
                // it has MaxVL == MaxPossibleVL
                if (computeVLMax(ELEN, VLEN, SEW, LMUL) == 0)
                  return true;
                return computeVLMax(ELEN, VLEN, SEW, LMUL) >= VL;
              });
      // Dist should not be normalized! Allowed to be empty, but has to be
      // present in the map.
      Distributions[VL] = Dist;
    }
  }

  const SewLmulDistribution &get(VLType VL) const {
    assert(Distributions.find(VL) != Distributions.end());
    return Distributions.at(VL);
  }
};

struct VLVMInfo {
  ProbableItems<VLGeneratorHolder> VLGens;
  ProbableItems<VMGeneratorHolder> VMGens;
  std::vector<std::string> DiscardedVLNames;
  std::vector<std::string> DiscardedVMNames;
};
} // namespace

void RVVConfigurationSpace::mapYaml(llvm::yaml::IO &IO,
                                    std::optional<RVVConfigurationSpace> &CS) {
  static_assert(std::is_copy_assignable_v<RVVConfigurationSpace>);
  yaml::EmptyContext Ctx;
  IO.mapOptionalWithContext(RVVConfigurationSpace::kUnitName, CS, Ctx);
}

class RVVConfig : public RVVConfigInterface {
public:
  RVVConfig() = default;
  RVVConfig(std::optional<RVVConfigurationSpace> &CS) : CS(CS) {}
  ~RVVConfig() override = default;

  bool hasConfig() const override { return CS.has_value(); }

  void mapYaml(yaml::IO &IO) override {
    RVVConfigurationSpace::mapYaml(IO, CS);
  }

  std::optional<RVVConfigurationSpace> getRVVConfigurationSpace() { return CS; }

private:
  std::optional<RVVConfigurationSpace> CS;
};

namespace llvm {

template <> struct yaml::MappingTraits<VXRMInfo> {
  static void mapping(yaml::IO &IO, VXRMInfo &VXRM) {
    IO.mapOptional("rnu", VXRM[VXRMMode::RNU], 0.0);
    IO.mapOptional("rne", VXRM[VXRMMode::RNE], 0.0);
    IO.mapOptional("rdn", VXRM[VXRMMode::RDN], 0.0);
    IO.mapOptional("ron", VXRM[VXRMMode::RON], 0.0);
  }

  static std::string validate(yaml::IO &IO, VXRMInfo &VXRM) {
    return checkWeights(make_second_range(VXRM), "VXRM");
  }
};

template <> struct yaml::MappingTraits<SEWInfo> {
  static void mapping(yaml::IO &IO, SEWInfo &SEW) {
    IO.mapOptional("sew_8", SEW[VSEW::SEW8], 0.0);
    IO.mapOptional("sew_16", SEW[VSEW::SEW16], 0.0);
    IO.mapOptional("sew_32", SEW[VSEW::SEW32], 0.0);
    IO.mapOptional("sew_64", SEW[VSEW::SEW64], 0.0);
  }

  static std::string validate(yaml::IO &IO, SEWInfo &SEW) {
    return checkWeights(make_second_range(SEW), "SEW");
  }
};

template <> struct yaml::MappingTraits<LMULInfo> {
  static void mapping(yaml::IO &IO, LMULInfo &LMUL) {
    IO.mapOptional("m1", LMUL[VLMUL::LMUL_1], 0.0);
    IO.mapOptional("m2", LMUL[VLMUL::LMUL_2], 0.0);
    IO.mapOptional("m4", LMUL[VLMUL::LMUL_4], 0.0);
    IO.mapOptional("m8", LMUL[VLMUL::LMUL_8], 0.0);
    IO.mapOptional("mf2", LMUL[VLMUL::LMUL_F2], 0.0);
    IO.mapOptional("mf4", LMUL[VLMUL::LMUL_F4], 0.0);
    IO.mapOptional("mf8", LMUL[VLMUL::LMUL_F8], 0.0);
  }

  static std::string validate(yaml::IO &IO, LMULInfo &LMUL) {
    return checkWeights(make_second_range(LMUL), "LMUL");
  }
};

template <> struct yaml::ScalarEnumerationTraits<VSEW> {
  static void enumeration(IO &IO, VSEW &Value) {
    IO.enumCase(Value, "sew_8", VSEW::SEW8);
    IO.enumCase(Value, "sew_16", VSEW::SEW16);
    IO.enumCase(Value, "sew_32", VSEW::SEW32);
    IO.enumCase(Value, "sew_64", VSEW::SEW64);
  }
};

template <> struct yaml::ScalarEnumerationTraits<VLMUL> {
  static void enumeration(IO &IO, VLMUL &Value) {
    IO.enumCase(Value, "m1", VLMUL::LMUL_1);
    IO.enumCase(Value, "m2", VLMUL::LMUL_2);
    IO.enumCase(Value, "m4", VLMUL::LMUL_4);
    IO.enumCase(Value, "m8", VLMUL::LMUL_8);
    IO.enumCase(Value, "mf2", VLMUL::LMUL_F2);
    IO.enumCase(Value, "mf4", VLMUL::LMUL_F4);
    IO.enumCase(Value, "mf8", VLMUL::LMUL_F8);
  }
};

template <> struct snippy::YAMLTupleTraits<SewLmulPair> {
  static auto members(SewLmulPair &E) {
    return std::tie(std::get<0>(E), std::get<1>(E));
  }

  // Note: It would be a perfect place to verify the validity of the
  // SEW LMUL pair here, but we don't have VLEN and ELEN available yet.
};
LLVM_SNIPPY_YAML_IS_TUPLE(SewLmulPair)
LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(SewLmulPair)

template <> struct yaml::MappingTraits<VMAInfo> {
  static void mapping(yaml::IO &IO, VMAInfo &VMA) {
    IO.mapOptional("mu", VMA[VMAMode::MU], 0.0);
    IO.mapOptional("ma", VMA[VMAMode::MA], 0.0);
  }

  static std::string validate(yaml::IO &IO, VMAInfo &VMA) {
    return checkWeights(make_second_range(VMA), "VMA");
  }
};

template <> struct yaml::MappingTraits<VTAInfo> {
  static void mapping(yaml::IO &IO, VTAInfo &VTA) {
    IO.mapOptional("tu", VTA[VTAMode::TU], 0.0);
    IO.mapOptional("ta", VTA[VTAMode::TA], 0.0);
  }

  static std::string validate(yaml::IO &IO, VTAInfo &VTA) {
    return checkWeights(make_second_range(VTA), "VTA");
  }
};

template <> struct yaml::MappingTraits<VTypeInfo> {
  static void mapping(yaml::IO &IO, VTypeInfo &VTYPE) {
    IO.mapOptional("SEW", VTYPE.SEW);
    IO.mapOptional("LMUL", VTYPE.LMUL);

    IO.mapRequired("VMA", VTYPE.VMA);
    IO.mapRequired("VTA", VTYPE.VTA);
  }
};

template <typename Policy>
std::string snippy::GeneratorName<Policy>::validate() const {
  auto GenOrErr = GeneratorFactory<typename Policy::Holder>::createOrErr(*this);
  if (!GenOrErr)
    return toString(GenOrErr.takeError());
  return {};
}

template <typename Policy>
struct yaml::ScalarTraits<snippy::GeneratorName<Policy>> {
  static void output(const snippy::GeneratorName<Policy> &Name, void *Ctx,
                     raw_ostream &OS) {
    ScalarTraits<std::string>::output(Name.asStr(), Ctx, OS);
  }
  static StringRef input(StringRef Scalar, void *Ctx,
                         snippy::GeneratorName<Policy> &Name) {
    return ScalarTraits<std::string>::input(Scalar, Ctx, Name.asStr());
  }
  static QuotingType mustQuote(StringRef) { return QuotingType::None; }
};

LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(
    snippy::GeneratorName<snippy::VLGeneratorPolicy>)
LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(
    snippy::GeneratorName<snippy::VMGeneratorPolicy>)
LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(
    snippy::GeneratorName<snippy::PrimaryDistBuilderPolicy>)

template <> struct yaml::MappingTraits<RVVUnitInfo> {
  static void mapping(yaml::IO &IO, RVVUnitInfo &VUInfo) {
    IO.mapRequired("VXRM", VUInfo.VXRM);
    IO.mapRequired("VTYPE", VUInfo.VTYPE);

    IO.mapOptional(snippy::VMGeneratorPolicy::Label, VUInfo.VM);
    IO.mapOptional(snippy::VLGeneratorPolicy::Label, VUInfo.VL);

    IO.mapOptional("mode-list", VUInfo.ModeList);

    IO.mapOptional(snippy::PrimaryDistBuilderPolicy::Label,
                   VUInfo.PrimaryBuilders);
  }

  static std::string validate(yaml::IO &IO, RVVUnitInfo &VUInfo) {
    bool IsSewSpecified = VUInfo.VTYPE.SEW.has_value();
    bool IsLmulSpecified = VUInfo.VTYPE.LMUL.has_value();
    bool IsModeListSpecified = !VUInfo.ModeList.empty();

    if ((IsSewSpecified || IsLmulSpecified) && IsModeListSpecified)
      return "It's not allowed to specify both SEW/LMUL configuration and a "
             "mode-list";
    if (IsSewSpecified && !IsLmulSpecified)
      return "Missing LMUL configuration for the specified SEW configuration";
    if (!IsSewSpecified && IsLmulSpecified)
      return "Missing SEW configuration for the specified LMUL configuration";
    if (!IsSewSpecified && !IsLmulSpecified && !IsModeListSpecified)
      return "Missing SEW and LMUL configuration or a mode-list";
    return {};
  }
};

static bool isCorrectProbability(double Prob) {
  return Prob >= 0.0 && Prob <= 1.0;
}

template <> struct yaml::ScalarTraits<ModeChangeP> {
  static StringRef input(StringRef Scalar, void *, ModeChangeP &P) {
    if (Scalar == ModeChangeP::ProbIsDeduced::Str) {
      P = ModeChangeP::ProbIsDeduced();
      return {};
    }
    double Value = 0.0;
    if (!Scalar.getAsDouble(Value)) {
      P = Value;
      return {};
    }

    // We don't have constexpr string concatenation, but at least check
    // that ErrStr contains ModeChangeP::ProbIsDeduced::Str;
    constexpr std::string_view ErrStr =
        "Invalid value for P. Expected \"deduced\" or a number [0.0;1.0]";
    static_assert(ErrStr.find(ModeChangeP::ProbIsDeduced::Str) !=
                  std::string_view::npos);
    return ErrStr;
  }

  static void output(const ModeChangeP &P, void *, raw_ostream &Out) {
    if (P.isDeduced())
      Out << ModeChangeP::ProbIsDeduced::Str;
    else
      Out << P.getAsDouble();
  }

  static QuotingType mustQuote(StringRef) { return QuotingType::None; }
};

template <> struct llvm::yaml::MappingTraits<ModeChangeBias> {
  static constexpr auto kProbBounds = "probability should be from [0.0;1.0]";

  static void mapping(IO &Io, ModeChangeBias &Guides) {
    Io.mapRequired("P", Guides.ModeChangeProb);
    Io.mapOptional("Pvill", Guides.SetVillP);
  }

  static std::string validate(yaml::IO &IO, ModeChangeBias &Guides) {
    if (!Guides.ModeChangeProb.isDeduced() &&
        !isCorrectProbability(Guides.ModeChangeProb.getAsDouble()))
      return std::string(RVVConfigurationSpace::kUnitName) + ": P " +
             kProbBounds;

    if (!isCorrectProbability(Guides.SetVillP))
      return std::string(RVVConfigurationSpace::kUnitName) + ": Pvill " +
             kProbBounds;
    return {};
  }
};

template <> struct yaml::MappingTraits<RVVConfigurationSpace> {
  static void mapping(yaml::IO &IO, RVVConfigurationSpace &Config) {
    IO.mapOptional("mode-change-bias", Config.Guides);
    IO.mapRequired("mode-distribution", Config.VUInfo);
  }
};

template <> struct yaml::MappingTraits<VectorUnitRules> {
  static void mapping(yaml::IO &IO, VectorUnitRules &VU) {
    IO.mapRequired(RVVConfigurationSpace::kUnitName, VU.Config);
  }
};
} // namespace llvm

namespace {
struct SewLmulPriorityBuilder final : PrimaryDistBuilderInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(PrimaryDistBuilderInterface,
                                              SewLmulPriorityBuilder,
                                              "sew_lmul")

  std::vector<double>
  buildWeights(unsigned ELEN, unsigned VLEN,
               const PrimaryConfigMapping &Mapping,
               const SewLmulDistribution &SewLmulDist,
               const ProbableItems<VLGeneratorHolder> &VLGenerators,
               bool IsForVSETIVLI) const override {
    std::vector<double> Weights(Mapping.maxIdx(), 0.0);

    // Construct VL distributions for each combination of {SEW, LMUL}
    VlDistributionStorage VLDistStorage(ELEN, VLEN, VLGenerators,
                                        IsForVSETIVLI);

    // For each {SEW, LMUL} pair valid VLs are [0, computeVLMax()]
    for (const auto &[SewLmul, SewLmulProb] : SewLmulDist) {
      auto [SEW, LMUL] = SewLmul;
      // Total weight of VLDist must be either 0.0 or 1.0
      auto VLDist = VLDistStorage.get(SEW, LMUL);
      assert(VLDist.size() <= Mapping.VLSize);

      for (unsigned VL = 0; VL < VLDist.size(); ++VL)
        Weights[Mapping.toIdx(SEW, LMUL, VL)] = VLDist[VL] * SewLmulProb;
    }
    return Weights;
  }
};

struct VlPriorityBuilder final : PrimaryDistBuilderInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(PrimaryDistBuilderInterface,
                                              VlPriorityBuilder, "vl")

  std::vector<double>
  buildWeights(unsigned ELEN, unsigned VLEN,
               const PrimaryConfigMapping &Mapping,
               const SewLmulDistribution &SewLmulDist,
               const ProbableItems<VLGeneratorHolder> &VLGenerators,
               bool IsForVSETIVLI) const override {
    std::vector<double> Weights(Mapping.maxIdx(), 0.0);

    SewLmulDistributionStorage SewLmulDistStorage(ELEN, VLEN, SewLmulDist);

    // Find the maximum possible VL across all given {SEW, LMUL} pairs.
    auto VLMaxRange =
        map_range(SewLmulDist, [&](const ProbableElement<SewLmulPair> &Item) {
          auto [SEW, LMUL] = Item.Element;
          return computeVLMax(ELEN, VLEN, SEW, LMUL);
        });
    unsigned MaxFeasibleVL = *max_element(VLMaxRange);
    auto VLDist =
        buildDistributionForMaxVL(MaxFeasibleVL, VLGenerators, IsForVSETIVLI);

    for (auto [VL, VlWeight] : enumerate(VLDist)) {
      auto Dist = SewLmulDistStorage.get(VL);
      if (Dist.empty())
        continue;
      // Divide by Dist.size() to avoid overcounting VL weight when multiple
      // {SEW, LMUL} pairs are legal for the same VL.
      VlWeight /= Dist.size();
      for (const auto &[Item, SewLmulP] : Dist) {
        auto [SEW, LMUL] = Item;
        Weights[Mapping.toIdx(SEW, LMUL, VL)] = VlWeight * SewLmulP;
      }
    }
    return Weights;
  }
};

struct UniformPriorityBuilder final : PrimaryDistBuilderInterface {
  DECLARE_ID_AND_DEFAULT_CONSTRUCTION_METHODS(PrimaryDistBuilderInterface,
                                              UniformPriorityBuilder,
                                              "3d_uniform")

  std::vector<double>
  buildWeights(unsigned ELEN, unsigned VLEN,
               const PrimaryConfigMapping &Mapping,
               const SewLmulDistribution &SewLmulDist,
               const ProbableItems<VLGeneratorHolder> &VLGenerators,
               bool IsForVSETIVLI) const override {
    std::vector<double> Weights(Mapping.maxIdx(), 0.0);

    // Construct VL distributions for each combination of {SEW, LMUL}
    VlDistributionStorage VLDistStorage(ELEN, VLEN, VLGenerators,
                                        IsForVSETIVLI);

    // For each {SEW, LMUL} pair valid VLs are [0, computeVLMax()]
    for (const auto &[SewLmul, SewLmulProb] : SewLmulDist) {
      auto [SEW, LMUL] = SewLmul;
      // Total weight of VLDist must be either 0.0 or 1.0
      auto VLDist = VLDistStorage.get(SEW, LMUL);
      assert(VLDist.size() <= Mapping.VLSize);

      for (unsigned VL = 0; VL < VLDist.size(); ++VL) {
        auto PossibleVLAmount =
            count_if(VLDist, [](double VlWeight) { return !isZero(VlWeight); });
        // Multiply by PossibleVLAmount. This makes it so the weight of each
        // {SEW, LMUL} pair is scaled by the number of possible VLs. For
        // example with VL=any_legal, PossibleVLAmount is VLMAX, so the
        // distribution shifts to be more uniform across all {SEW, LMUL, VL}
        // triplets.
        Weights[Mapping.toIdx(SEW, LMUL, VL)] =
            VLDist[VL] * SewLmulProb * PossibleVLAmount;
      }
    }
    return Weights;
  }
};

template <> struct GeneratorFactory<PrimaryDistBuilderHolder> {
  static Expected<PrimaryDistBuilderHolder> createOrErr(StringRef ID) {
    return constructFromString<PrimaryDistBuilderInterface,
                               SewLmulPriorityBuilder, VlPriorityBuilder,
                               UniformPriorityBuilder>(ID);
  }

  static PrimaryDistBuilderHolder create(StringRef ID) {
    return cantFail(createOrErr(ID));
  }
};
} // namespace

namespace llvm {
namespace snippy {
RVVPrimaryConfigGenerator::RVVPrimaryConfigGenerator(
    unsigned ELEN, unsigned VLEN, const SewLmulDistribution &SewLmulDist,
    const ProbableItems<VLGeneratorHolder> &VLGenerators,
    unsigned MinVMBitWidth, bool IsForVSETIVLI,
    const ProbableItems<PrimaryDistBuilderHolder> &PrimaryDistBuilders)
    : VLSize(IsForVSETIVLI ? (kMaxVLForVSETIVLI + 1)
                           : (getMaxPossibleVL(ELEN, VLEN) + 1)),
      IsForVSETIVLI(IsForVSETIVLI), Mapping(VLSize) {
  assert(VLEN != 0 && ELEN != 0);

  assert(SewLmulDist.hasNonZeroProbs());
  assert(VLGenerators.hasNonZeroProbs());
  assert(PrimaryDistBuilders.hasNonZeroProbs());

  std::vector<double> Weights(Mapping.maxIdx(), 0.0);
  // Add weights from each builder
  for (const auto &[Builder, Prob] : PrimaryDistBuilders) {
    if (isZero(Prob))
      continue;
    auto BuilderWeights = Builder->buildWeights(
        ELEN, VLEN, Mapping, SewLmulDist, VLGenerators, IsForVSETIVLI);

    if (all_of(BuilderWeights, [](double W) { return isZero(W); }))
      continue;
    // The sum of BuilderWeights must be 1.0
    normalizeValues(BuilderWeights);

    for (auto &&[Weight, BuilderWeight] : zip(Weights, BuilderWeights))
      Weight += BuilderWeight * Prob;
  }

  // Zero-out weights of VLs < minVMBitWidth.
  // For such configs there're no applicable VMgens.
  for (auto &&[Idx, Weight] : enumerate(Weights)) {
    auto VL = idxToConfig(Idx).VL;
    if (VL < MinVMBitWidth)
      Weight = 0.0;
  }

  if (all_of(Weights, [](double W) { return isZero(W); })) {
    if (IsForVSETIVLI)
      snippy::fatal(
          "There are no VL generators in riscv-vector-unit which are "
          "compatible with VSETIVLI under any requested VTYPE configuration");
    snippy::fatal("There are no VL generators in riscv-vector-unit which are "
                  "compatible with any requested VTYPE configuration");
  }

  Dist = std::discrete_distribution<unsigned>(Weights.begin(), Weights.end());

  LLVM_DEBUG(dbgs() << "=== Primary RVV Config Probabilities ===\n");
  LLVM_DEBUG(printProbabilities(dbgs()));
  LLVM_DEBUG(dbgs() << "=== Final {SEW, LMUL} Probabilities ===\n");
  LLVM_DEBUG(printCombinedSewLmulProbs(dbgs(), getAllPossibleSewLmulPairs()));
}

RVVPrimaryConfig RVVPrimaryConfigGenerator::generate() const {
  size_t Idx = Dist(RandEngine::engine());
  return idxToConfig(Idx);
}

ProbableItems<RVVPrimaryConfig>
PrimaryWeightsAndMapping::getAllPossibleConfigs() const {
  ProbableItems<RVVPrimaryConfig> Result;
  // Add to result only points with non-zero probabilities
  for (const auto &[Idx, Prob] : enumerate(Weights)) {
    if (!isZero(Prob))
      Result.emplace_back(Mapping.idxToConfig(Idx), Prob);
  }
  assert(Result.checkSumOfProbabilities());
  return Result;
}

SewLmulDistribution
PrimaryWeightsAndMapping::getAllPossibleSewLmulPairs() const {
  SewLmulDistribution Result;
  const auto Cfgs = getAllPossibleConfigs();
  for (const auto &[Cfg, Prob] : Cfgs) {
    const auto SEW = Cfg.SEW;
    const auto LMUL = Cfg.LMUL;

    Result.getProbOrEmplace({SEW, LMUL}) += Prob;
  }
  assert(Result.checkSumOfProbabilities());
  return Result;
}

void PrimaryWeightsAndMapping::printProbabilities(raw_ostream &OS) const {
  constexpr unsigned SewWidth = 10;
  constexpr unsigned LmulWidth = 10;
  constexpr unsigned VLWidth = 7;
  constexpr unsigned ProbWidth = 9;

  OS << right_justify("SEW", SewWidth) << right_justify("LMUL", LmulWidth)
     << right_justify("VL", VLWidth) << right_justify("Prob,%", ProbWidth);

  std::optional<VSEW> PrevSEW;
  std::optional<VLMUL> PrevLMUL;

  for (const auto &[Cfg, Prob] : getAllPossibleConfigs()) {
    assert(!isZero(Prob));
    auto [SEW, LMUL, VL] = Cfg;

    // Separate by {SEW, LMUL}
    if (SEW != PrevSEW || LMUL != PrevLMUL) {
      OS << "\n";
      PrevSEW = SEW;
      PrevLMUL = LMUL;
    }

    OS << right_justify(toString(SEW), SewWidth)
       << right_justify(toString(LMUL), LmulWidth)
       << right_justify(std::to_string(VL), VLWidth)
       << format("%*.3f", ProbWidth, Prob * 100) << "\n";
  }
  OS << "\n";
}

RVVConfiguration RVVConfigGenerator::generate(bool MustUseReducedVL) const {
  assert((!MustUseReducedVL || PrimaryGenReduced) &&
         "Requested to sample a mode for VSETIVLI but there "
         "is no generator for it");
  assert((MustUseReducedVL || PrimaryGen) &&
         "Requested to sample a mode for VSETVLI or VSETVL but "
         "there is no generator for them");
  // Weights of VSET opcodes can affect the sum of two primary distributions
  // (which we get when sampling with this generator). In the future, to
  // account for this, we can shift marginal VL distribution to smaller values
  // for VSETIVLI and to larger for VSETVL & VSETVLI.
  //
  // But this problem has some mathematical limitations:
  // For example, if we have histogram:
  //   - [VSETVL, 0.001]
  //   - [VSETIVLI, 0.999]
  // It's impossible to satisfy
  // VL:
  //   - [100, 0.999]
  //   - [1, 0.001]
  auto PrimaryCfg =
      MustUseReducedVL ? PrimaryGenReduced->generate() : PrimaryGen->generate();
  auto MA = VmaGen.generate();
  auto TA = VtaGen.generate();
  auto XRM = VxrmGen.generate();

  return RVVConfiguration{PrimaryCfg, MA, TA, XRM};
}

PrimaryWeightsAndMapping RVVConfigGenerator::getCombinedDistribution(
    const ModeChangeInfo &SwitchInfo) const {
  assert(PrimaryGen || PrimaryGenReduced);
  if (!PrimaryGenReduced)
    return PrimaryGen->getWeightsAndMapping();
  if (!PrimaryGen)
    return PrimaryGenReduced->getWeightsAndMapping();

  auto [ProbVSETVL, ProbVSETVLI, ProbVSETIVLI] =
      SwitchInfo.getRelativeProbabilities();

  bool IsReducedGenLarger = PrimaryGen->VLSize > PrimaryGenReduced->VLSize;
  const auto &LargerGen =
      !IsReducedGenLarger ? *PrimaryGenReduced : *PrimaryGen;
  const auto &SmallerGen =
      IsReducedGenLarger ? *PrimaryGenReduced : *PrimaryGen;
  double LargerMult =
      IsReducedGenLarger ? ProbVSETIVLI : ProbVSETVL + ProbVSETVLI;
  double SmallerMult = 1.0 - LargerMult;

  auto LargerWeights = LargerGen.getWeightsAndMapping();
  auto SmallerWeights = SmallerGen.getWeightsAndMapping();

  LargerWeights *= LargerMult;
  SmallerWeights *= SmallerMult;
  LargerWeights += SmallerWeights;

  return LargerWeights;
}

std::unique_ptr<RVVConfigInterface> createRVVConfig() {
#if 0
  initRISCVTargetParserOptions();
  if (AllowReservedSEW == cl::BOU_UNSET)
    // Snippy allows using reserved SEW (128 - 1024) encodings and RISC-V LLVM
    // backend should be able to encode it.
    AllowReservedSEW = cl::BOU_TRUE;
#endif
  AllowReservedSEW = true;
  return std::make_unique<RVVConfig>();
}

bool isLegalSewLmul(unsigned ELEN, unsigned VLEN, VSEW SEW, VLMUL LMUL) {
  if (!isLegalLMUL(LMUL))
    return false;
  if (!isLegalSEW(SEW))
    return false;

  unsigned SEWVal = static_cast<unsigned>(SEW);
  if (SEWVal > ELEN)
    return false;

  auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);

  if (IsFractional) {
    unsigned MinFracMultiplier = ELEN / SEWVal;
    if (Multiplier > MinFracMultiplier)
      return false;

    if (VLEN / SEWVal < Multiplier)
      return false;
  }
  return true;
}

unsigned computeVLMax(unsigned ELEN, unsigned VLEN, VSEW VSEW, VLMUL LMUL) {
  if (!isLegalSewLmul(ELEN, VLEN, VSEW, LMUL))
    return 0;

  auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
  unsigned SEW = static_cast<unsigned>(VSEW);

  unsigned Result =
      IsFractional ? (VLEN / SEW / Multiplier) : (VLEN / SEW * Multiplier);
  assert(Result > 0 && "At this point SEW and LMUL are known to be legal");
  return Result;
}

inline static bool isReservedValues(unsigned ELEN, unsigned SEW, VLMUL LMUL) {
  if (LMUL == VLMUL::LMUL_RESERVED)
    return true;
  auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
  auto MinFracMultiplier = ELEN / SEW;
  return !isLegalSEW(SEW) || (SEW > ELEN) ||
         (IsFractional && (Multiplier > MinFracMultiplier));
}

std::pair<unsigned, bool> computeDecodedEMUL(unsigned ELEN, unsigned SEW,
                                             unsigned EEW, VLMUL LMUL) {
  if (isReservedValues(ELEN, SEW, LMUL) || !isLegalSEW(SEW) ||
      !isLegalSEW(EEW)) {
    // Calculating EMUL doesn't make sense for illegal values of SEW or LMUL, so
    // just return {1, 0}
    return {1, 0};
  }

  auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
  unsigned long long Dividend = EEW * (IsFractional ? 1u : Multiplier);
  unsigned long long Divisor = SEW * (IsFractional ? Multiplier : 1u);
  if (Dividend < Divisor)
    return {Divisor / Dividend, /* fractional */ true};
  return {Dividend / Divisor, /* fractional */ false};
}

bool isValidEMUL(unsigned ELEN, unsigned SEW, unsigned EEW, VLMUL LMUL) {
  auto [EMUL, IsFractional] = computeDecodedEMUL(ELEN, SEW, EEW, LMUL);
  return RISCVVType::isValidLMUL(EMUL, IsFractional);
}

VLMUL computeEMUL(unsigned ELEN, unsigned SEW, unsigned EEW, VLMUL LMUL) {
  auto [EMUL, IsFractional] = computeDecodedEMUL(ELEN, SEW, EEW, LMUL);
  assert(RISCVVType::isValidLMUL(EMUL, IsFractional));
  return RISCVVType::encodeLMUL(EMUL, IsFractional);
}

std::string RVVConfiguration::toStr() const {
  constexpr unsigned SewWidth = 3;
  constexpr unsigned LmulWidth = 3;
  constexpr unsigned VLWidth = 3;

  auto SewStr = toString(PrimaryCfg.SEW);
  auto LmulStr = toString(PrimaryCfg.LMUL);
  auto VLStr = std::to_string(PrimaryCfg.VL);

  std::string Result;
  raw_string_ostream OS(Result);
  OS << "[" << right_justify(SewStr, SewWidth) << ", "
     << right_justify(LmulStr, LmulWidth) << ", "
     << right_justify(VLStr, VLWidth) << ", " << toString(MA) << ", "
     << toString(TA) << ", " << toString(XRM) << "]";
  return Result;
}

static unsigned getNumReservedSEW(unsigned SEW) {
  switch (static_cast<VSEW>(SEW)) {
  default:
    return 0;
  case VSEW::SEWReserved1:
    return 1;
  case VSEW::SEWReserved2:
    return 2;
  case VSEW::SEWReserved3:
    return 3;
  case VSEW::SEWReserved4:
    return 4;
  }
}

static void printOldStyleConfig(raw_ostream &OS, VSEW SEW, VLMUL LMUL,
                                VTAMode TA, VMAMode MA, VXRMMode XRM) {
  OS << "{ ";
  unsigned SewVal = static_cast<unsigned>(SEW);
  OS << "e";
  if (!isLegalSEW(SEW))
    OS << "Reserved" << getNumReservedSEW(SewVal);
  else
    OS << SewVal;

  if (LMUL == VLMUL::LMUL_RESERVED) {
    OS << ", mReserved";
  } else {
    auto [LMulVal, Fractional] = RISCVVType::decodeVLMUL(LMUL);
    OS << (Fractional ? ", mf" : ", m") << LMulVal;
  }

  OS << (TA == VTAMode::TA ? ", ta" : ", tu");
  OS << (MA == VMAMode::MA ? ", ma" : ", mu");
  OS << ", vxrm: " << toString(XRM);
  OS << " }";
}

// Computes {SEW, LMUL} pairs that have non-zero user-specified weight but
// zero probability in the final distribution (i.e. no VL generator was
// applicable). Returns them expanded with all non-zero MA/TA/VXRM combos.
static std::vector<std::tuple<VSEW, VLMUL, VTAMode, VMAMode, VXRMMode>>
computeDiscardedConfigs(unsigned ELEN, unsigned VLEN, const RVVUnitInfo &VUInfo,
                        const PrimaryWeightsAndMapping &WeightsAndMapping) {
  const auto &SewLmulPairs = WeightsAndMapping.getAllPossibleSewLmulPairs();

  const auto &RawSewLmulPairs = std::invoke([&] {
    if (!VUInfo.ModeList.empty()) {
      assert(!VUInfo.VTYPE.SEW.has_value() && !VUInfo.VTYPE.LMUL.has_value());
      auto Result = VUInfo.ModeList;
      Result.squashSame();
      return Result;
    }
    assert(VUInfo.VTYPE.SEW.has_value() && VUInfo.VTYPE.LMUL.has_value());
    auto SEWNorm = normalizeWeights(*VUInfo.VTYPE.SEW);
    auto LMULNorm = normalizeWeights(*VUInfo.VTYPE.LMUL);
    return jointProbabilityDistribution(SEWNorm, LMULNorm);
  });

  auto MANorm = normalizeWeights(VUInfo.VTYPE.VMA);
  auto TANorm = normalizeWeights(VUInfo.VTYPE.VTA);
  auto XRMNorm = normalizeWeights(VUInfo.VXRM);
  const auto &SecondaryCombos =
      jointProbabilityDistribution(MANorm, TANorm, XRMNorm);

  std::vector<std::tuple<VSEW, VLMUL, VTAMode, VMAMode, VXRMMode>> Result;
  for (const auto &[SewLmul, Prob] : RawSewLmulPairs) {
    if (isZero(Prob))
      continue; // zero weight was user-specified, not "discarded"
    const auto &[SEW, LMUL] = SewLmul;
    if (!isLegalSewLmul(ELEN, VLEN, SEW, LMUL))
      continue; // illegal, not "discarded"
    if (SewLmulPairs.contains(SewLmul))
      continue; // present in the final distribution

    // At this point we know that {SEW, LMUL} was "requested" by the user but
    // not present in the final distribution. Expand it with MA/TA/VXRM combos.
    for (const auto &[Item, Prob] : SecondaryCombos) {
      const auto &[MA, TA, XRM] = Item;
      Result.emplace_back(SEW, LMUL, TA, MA, XRM);
    }
  }
  return Result;
}

static void
printDiscardedRVVConfigurationsImpl(raw_ostream &OS,
                                    const RVVConfigurationInfo &RVVCfg) {
  OS << "--- RVV Unit Discarded Info ---\n";
  if (!RVVCfg.isRVVEnabled()) {
    OS << "None\n";
    OS << "--- RVV Unit Discarded End  ---\n\n";
    return;
  }

  // Access internal generator info via the public getAllPossiblePrimaryConfigs
  // proxy; discarded VM/VL names are stored directly on GenInfo.
  // We reach them through the RVVConfigurationInfo interface.
  const auto *GenInfoPtr = RVVCfg.getGenInfoPtr();
  assert(GenInfoPtr && "GenInfo must be present when RVV is enabled");
  const auto &GenInfo = *GenInfoPtr;

  OS << "  - VM Generators:\n";
  for (const auto &Name : GenInfo.SupportInfo.DiscardedVMNames)
    OS << "    <" << Name << ">\n";

  OS << "  - VL Generators:\n";
  for (const auto &Name : GenInfo.SupportInfo.DiscardedVLNames)
    OS << "    <" << Name << ">\n";

  const auto &VUInfo = GenInfo.SupportInfo.OriginalVUInfo;
  unsigned VLEN = RVVCfg.getVLEN();
  unsigned ELEN = RVVCfg.getELEN();
  OS << "  - Configuration Bag Listing:\n";

  auto DiscardedCfgs = computeDiscardedConfigs(
      ELEN, VLEN, VUInfo,
      GenInfo.CfgGen.getCombinedDistribution(RVVCfg.getModeChangeInfo()));
  for (const auto &[SEW, LMUL, TA, MA, XRM] : DiscardedCfgs) {
    OS << "    Conf: ";
    printOldStyleConfig(OS, SEW, LMUL, TA, MA, XRM);
    OS << "\n";
  }

  OS << "--- RVV Unit Discarded End  ---\n\n";
}

static void
printDiscardedRVVConfigurations(const RVVConfigurationInfo &RVVCfg) {
  StringRef FilePath = DumpDiscardedRVVConfigurations.getValue();
  std::string Filename = FilePath.empty() ? "-" : FilePath.str();
  if (Error Err = checkedWriteToOutput(Filename, [&](raw_ostream &OS) {
        printDiscardedRVVConfigurationsImpl(OS, RVVCfg);
        return Error::success();
      }))
    snippy::fatal("riscv-dump-discarded-rvv-configurations error : " +
                  toString(std::move(Err)));
}

static bool hasVXRMUsers(const OpcodeHistogram &Hist) {
  return llvm::any_of(Hist.uniqueOpcodes(),
                      [](auto Opcode) { return isRVVuseVXRM(Opcode); });
}

static RVVConfigurationSpace createDefaultConfigurationSpace() {
  SEWInfo SEW;
  LMULInfo LMUL;
  VMAInfo VMA;
  VTAInfo VTA;
  VXRMInfo VXRM;

  // Default vector unit info. Will always use the config that represents all
  // bits of `vtype` field set to zero and LMUL=1, SEW=64.
  SEW[VSEW::SEW64] = 1.0;
  LMUL[VLMUL::LMUL_1] = 1.0;
  VMA[VMAMode::MU] = 1.0;
  VTA[VTAMode::TU] = 1.0;
  VXRM[VXRMMode::RNU] = 1.0;
  VMSequence VMSeq = {{UnmaskedVMGenerator::kID, 1.0}};
  VLSequence VLSeq = {{MaxPossibleVLGen::kID, 1.0}};

  VTypeInfo VTYPE{SEW, LMUL, VMA, VTA};
  RVVUnitInfo VUInfo{
      VXRM, VTYPE, VMSeq, VLSeq, SewLmulDistribution{}, /*PrimaryBuilders=*/{}};
  RVVConfigurationSpace CS{
      /*no bias, deduce P from histogram*/ ModeChangeBias{}, VUInfo};

  return CS;
}

// MinVL = minimum bit width of all VM generators
static unsigned getMinRequestedBitWidth(const VMSequence &VMSeq) {
  assert(!VMSeq.empty());
  unsigned MinVL = std::numeric_limits<unsigned>::max();
  for (const auto &[Name, Weight] : VMSeq) {
    auto VMGen = GeneratorFactory<VMGeneratorHolder>::create(Name.asStr());
    MinVL = std::min(MinVL, VMGen->getMinRequiredVL());
  }
  return MinVL;
}

// For most VL generators, if it's applicable for a given {SEW, LMUL} pair, it
// will be applicable for any {SEW, LMUL} pair that has greater VLMax. So we
// could find the minimum VLMax of all {SEW, LMUL} pairs and check only it.
//
// Sadly, this is not the case for 'vlmax' generator (MaxVLGenerator),
// so we have to check all {SEW, LMUL} pairs.
//
// Returns std::nullopt if the given VLGen is not applicable for any {SEW, LMUL}
// from the SewLmulDist
static std::optional<std::pair</*MinVL*/ unsigned, /*MaxVL*/ unsigned>>
getMinMaxPossibleVLOfGenerator(unsigned ELEN, unsigned VLEN,
                               const VLGeneratorHolder &VLGen,
                               const SewLmulDistribution &SewLmulDist) {
  unsigned MaxPossibleVL = getMaxPossibleVL(ELEN, VLEN);

  // If all configurations are illegal we allow all VL gens.
  if (none_of(SewLmulDist.getItemsRange(), [=](const auto &SewLmul) {
        const auto &[SEW, LMUL] = SewLmul;
        return isLegalSewLmul(ELEN, VLEN, SEW, LMUL);
      }))
    return {{0, MaxPossibleVL}};

  VLDistributionType FinalHist(MaxPossibleVL + 1);
  for (const auto &[SewLmul, Weight] : SewLmulDist) {
    assert(!isZero(Weight));

    const auto &[SEW, LMUL] = SewLmul;
    unsigned VLMax = computeVLMax(ELEN, VLEN, SEW, LMUL);
    if (VLMax == 0) // config is illegal
      continue;

    // Never using here VLGen->getDistributionForVSETIVLI(VLMax) because at the
    // moment it squashes weights from VL > kMaxVLForVSETIVLI to
    // kMaxVLForVSETIVLI. This can make it so the distribution is not empty even
    // if it should be.
    const auto &Hist = VLGen->getDistribution(VLMax);

    // Note: zip will iterate till the shortest container
    assert(FinalHist.size() >= Hist.size());
    for (auto [FinalW, W] : zip(FinalHist, Hist))
      FinalW += W;
  }

  if (all_of(FinalHist, [](double W) { return isZero(W); }))
    return std::nullopt;

  return {{getIdxFirstNonZero(FinalHist), getIdxLastNonZero(FinalHist)}};
}

// Leaves only VMs and VLs for which there is at least one compatible VL/VM
// generator. For example:
//   - VM1 = 0b1001  (4 bits)
//   - VM2 = 0b11010 (5 bits)
//
//   - VL1 = 1
//   - VL2 = 4
// VM2 must be discarded since there is no VL generator that can do 5 bits
// VL1 must be discarded since there is no VM generator that can do 1 bit VM
static VLVMInfo buildVLVMgenerators(unsigned ELEN, unsigned VLEN,
                                    const VLSequence &OriginalVLSeq,
                                    const VMSequence &OriginalVMSeq,
                                    const SewLmulDistribution &SewLmulDist,
                                    bool IsOnlyVSETIVLI) {
  VLVMInfo Result;

  // We discard all VL generators for which there is no available VM
  // generator (MinBitWidth > MaxVL of this generator).
  unsigned MinRequestedBitWidth = getMinRequestedBitWidth(OriginalVMSeq);

  unsigned GlobalMaxVL = 0;
  for (const auto &[Name, Weight] : OriginalVLSeq) {
    auto VLGen = GeneratorFactory<VLGeneratorHolder>::create(Name.asStr());
    auto MinMaxVLOpt =
        getMinMaxPossibleVLOfGenerator(ELEN, VLEN, VLGen, SewLmulDist);

    unsigned MinVL = 0, MaxVL = 0;
    bool NeedToDiscard = std::invoke([&] {
      if (!MinMaxVLOpt.has_value())
        return true;
      std::tie(MinVL, MaxVL) = MinMaxVLOpt.value();
      // Some terrible logic here: we don't discard 'max_encodable' VL generator
      // when it's MinVL > kMaxVLForVSETIVLI. That's the difference between
      // 'max_encodable' and 'vlmax'.
      return MaxVL < MinRequestedBitWidth ||
             (IsOnlyVSETIVLI && MinVL > kMaxVLForVSETIVLI &&
              VLGen->identify() != MaxPossibleVLGen::kID);
    });

    if (NeedToDiscard) {
      Result.DiscardedVLNames.push_back(VLGen->identify());
    } else {
      Result.VLGens.emplace_back(std::move(VLGen), Weight);
      GlobalMaxVL = std::max(GlobalMaxVL, MaxVL);
    }
  }

  // We also discard all VM generators for which there is no available VL
  // generator (GlobalMaxVL < BitWidth of this generator).
  for (const auto &[Name, Weight] : OriginalVMSeq) {
    auto VMGen = GeneratorFactory<VMGeneratorHolder>::create(Name.asStr());
    unsigned MinVMBitWidth = VMGen->getMinRequiredVL();
    if (MinVMBitWidth > GlobalMaxVL)
      Result.DiscardedVMNames.push_back(VMGen->identify());
    else
      Result.VMGens.emplace_back(std::move(VMGen), Weight);
  }

  if (Result.VLGens.empty())
    snippy::fatal(
        "riscv-vector-unit: Could not find any applicable VL generators");
  if (Result.VMGens.empty())
    snippy::fatal(
        "riscv-vector-unit: Could not find any applicable VM generators");

  Result.VLGens.normalizeProbs();
  Result.VMGens.normalizeProbs();

  LLVM_DEBUG({
    dbgs() << "=== RVV VM Generators ===\n";
    Result.VMGens.print(dbgs(),
                        [](const VMGeneratorHolder &Gen) -> std::string {
                          return Gen->identify();
                        });
    dbgs() << "=== RVV VL Generators ===\n";
    Result.VLGens.print(dbgs(),
                        [](const VLGeneratorHolder &Gen) -> std::string {
                          return Gen->identify();
                        });
    dbgs() << "=== RVV Discarded VM Generators ===\n";
    if (Result.DiscardedVMNames.empty())
      dbgs() << "None\n";
    for (const auto &Name : Result.DiscardedVMNames)
      dbgs() << Name << "\n";
    dbgs() << "\n=== RVV Discarded VL Generators ===\n";
    if (Result.DiscardedVLNames.empty())
      dbgs() << "None\n";
    for (const auto &Name : Result.DiscardedVLNames)
      dbgs() << Name << "\n";
    dbgs() << "\n";
  });

  return Result;
}

static ProbableItems<PrimaryDistBuilderHolder>
buildPrimaryDistributionBuilders(const PrimaryDistBuilderSequence &Seq) {
  ProbableItems<PrimaryDistBuilderHolder> Result;
  if (Seq.empty()) {
    // By default fall back to SewLmulPriority. (old behavior, backward
    // compatibility)
    auto Builder = GeneratorFactory<PrimaryDistBuilderHolder>::create(
        SewLmulPriorityBuilder::kID);
    Result.emplace_back(std::move(Builder), /*Prob*/ 1.0);
  } else {
    for (const auto &[Name, Weight] : Seq) {
      auto Builder =
          GeneratorFactory<PrimaryDistBuilderHolder>::create(Name.asStr());
      Result.emplace_back(std::move(Builder), Weight);
    }
    Result.normalizeProbs();
  }

  LLVM_DEBUG({
    dbgs() << "=== RVV Primary Distribution Builders ===\n";
    Result.print(dbgs(),
                 [](const PrimaryDistBuilderHolder &Gen) -> std::string {
                   return Gen->identify();
                 });
  });

  assert(Result.checkSumOfProbabilities());
  return Result;
}

RVVConfigurationInfo RVVConfigurationInfo::buildConfiguration(const Config &Cfg,
                                                              unsigned ELEN,
                                                              unsigned VLEN) {
  auto CSOpt = Cfg.ProgramCfg.TargetConfig->getImpl<RISCVConfigInterface>()
                   .RVVConfig->getImpl<RVVConfig>()
                   .getRVVConfigurationSpace();
  if (CSOpt && VLEN == 0)
    snippy::fatal("RVV configuration file should not be "
                  "specified for targets without RVV");
  const auto &CS = CSOpt ? CSOpt.value() : createDefaultConfigurationSpace();

  auto SwitchInfo = deriveModeSwitchingProbability(Cfg, CS.Guides);
  bool IsArtificialModeChange = !CS.Guides.ModeChangeProb.isDeduced();
  bool NeedsVXRMUpdate = hasVXRMUsers(Cfg.getOpcodeHistogram());

  if (VLEN == 0)
    return RVVConfigurationInfo{ELEN, VLEN, std::move(SwitchInfo),
                                IsArtificialModeChange, NeedsVXRMUpdate};

  const auto &SewLmulDist = buildRawSewLmulDistribution(ELEN, VLEN, CS.VUInfo,
                                                        SwitchInfo.ProbSetVill);

  bool IsPresentVSETIVLI = SwitchInfo.hasVSETIVLI();
  bool IsPresentNonVSETIVLI = SwitchInfo.hasVSETVL() || SwitchInfo.hasVSETVLI();
  assert(IsPresentVSETIVLI || IsPresentNonVSETIVLI);

  // At *some* point we HAVE TO exclude all VL values that don't have any
  // compatible VMgen. Currently this is done here, by discarding all VLgens
  // that don't have any applicable VMgens. It would be better to discard
  // individual VL values, and do this inside RVVPrimaryConfigGenerator
  // constructor.
  // Everything else that is done here, like discarding VLgens for which
  // there're no {SEW, LMUL} combinations or discarding VMgens, is UNNECESSARY
  // and is done only to get discarded names (backward compatibility).
  auto [VLGenerators, VMGenerators, DiscardedVLNames, DiscardedVMNames] =
      buildVLVMgenerators(ELEN, VLEN, CS.VUInfo.VL, CS.VUInfo.VM, SewLmulDist,
                          /*IsOnlyVSETIVLI*/ !IsPresentNonVSETIVLI);

  auto PrimaryDistBuilders =
      buildPrimaryDistributionBuilders(CS.VUInfo.PrimaryBuilders);

  unsigned MinVMBitWidth = getMinRequestedBitWidth(CS.VUInfo.VM);
  // One generator for (VSETVLI & VSETVL) and another for VSETIVLI

  std::optional<RVVPrimaryConfigGenerator> PrimaryConfigGen;
  if (IsPresentNonVSETIVLI)
    PrimaryConfigGen.emplace(ELEN, VLEN, SewLmulDist, VLGenerators,
                             MinVMBitWidth, /*BuildForVSETIVLI*/ false,
                             PrimaryDistBuilders);

  std::optional<RVVPrimaryConfigGenerator> PrimaryConfigGenReduced;
  if (IsPresentVSETIVLI)
    PrimaryConfigGenReduced.emplace(
        ELEN, VLEN, SewLmulDist, VLGenerators, MinVMBitWidth,
        /*BuildForVSETIVLI*/ true, PrimaryDistBuilders);

  RVVConfigGenerator RVVConfigGen(
      std::move(PrimaryConfigGen), std::move(PrimaryConfigGenReduced),
      CS.VUInfo.VTYPE.VMA, CS.VUInfo.VTYPE.VTA, CS.VUInfo.VXRM);
  RVVSupportInfo SupportInfo(CS.VUInfo, std::move(DiscardedVMNames),
                             std::move(DiscardedVLNames));

  return RVVConfigurationInfo{ELEN,
                              VLEN,
                              std::move(SwitchInfo),
                              IsArtificialModeChange,
                              NeedsVXRMUpdate,
                              RVVGeneratorInfo(std::move(RVVConfigGen),
                                               std::move(VMGenerators),
                                               std::move(SupportInfo))};
}

APInt RVVConfigurationInfo::selectVM(unsigned VL) const {
  assert(GenInfo && "There must be a generator. Probably VLEN is 0.");
  auto Filter = [VL](const VMGeneratorHolder &VMGen) {
    return VMGen->getMinRequiredVL() <= VL;
  };
  // Generation under a condition can return error only if all elements
  // do not satisfy this condition. This can't be the case, since we have
  // already thrown out all VMs for which there is no available VL.
  const auto &ApplicableGen = cantFail(GenInfo->VMGen.generateIf(Filter));
  return ApplicableGen->generate(VL);
}

RVVConfiguration
RVVConfigurationInfo::selectConfiguration(bool MustUseReducedVL) const {
  assert(GenInfo && "There must be a generator. Probably VLEN is 0.");
  auto Cfg = GenInfo->CfgGen.generate(MustUseReducedVL);

  assert(!MustUseReducedVL || Cfg.PrimaryCfg.VL <= kMaxVLForVSETIVLI);
  return Cfg;
}

ProbableItems<RVVPrimaryConfig>
RVVConfigurationInfo::getAllPossiblePrimaryConfigs() const {
  assert(GenInfo && "There must be a generator. Probably VLEN is 0.");

  return GenInfo->CfgGen.getCombinedDistribution(SwitchInfo)
      .getAllPossibleConfigs();
}

// This print functionality is kept for backward compatibility.
// It barely represents the current state of the configuration.
// We actually have more information than we print, for example
// probability of each {SEW, LMUL, VL} combination.
void RVVConfigurationInfo::print(raw_ostream &OS) const {
  OS << "--- RVV Configuration Info ---\n";
  OS << "  - Derived VLEN: " << VLEN << " (VLENB = " << VLEN / RISCV_CHAR_BIT
     << ")\n";
  OS << "  - Mode Change Decision Policy: ";

  if (!SwitchInfo.RVVPresentInHistogram) {
    OS << "None\n";
    OS << "--- RVV Configuration End  ---\n\n";
    return;
  }
  assert(GenInfo && "There must be a generator. Probably VLEN is 0.");
  const auto &VMGen = GenInfo->VMGen;
  const auto &CfgGen = GenInfo->CfgGen;

  if (ArtificialModeChange)
    OS << "Configuration Bias\n";
  else
    OS << "Histogram\n";

  auto Mult = SwitchInfo.getWeightToProbabilityMultiplier();
  auto TotalWeight = SwitchInfo.WeightVSETVL + SwitchInfo.WeightVSETVLI +
                     SwitchInfo.WeightVSETIVLI;
  OS << formatv("  - Mode Change Probability: {0:F3} "
                "(vsetvl/vsetvli/vsetivli={1:F3}/{2:F3}/{3:F3})\n",
                TotalWeight * Mult, SwitchInfo.WeightVSETVL * Mult,
                SwitchInfo.WeightVSETVLI * Mult,
                SwitchInfo.WeightVSETIVLI * Mult);
  OS << "    ";
  OS << "Set Vill Bit Probability: " << floatToString(SwitchInfo.ProbSetVill, 3)
     << "\n";

  // Simply print values from the OriginalVUInfo but exclude the
  // discarded ones.
  const auto &DiscardedVLNames = GenInfo->SupportInfo.DiscardedVLNames;
  ProbableItems<std::string> VLGensNames;
  for (const auto &[GenStr, Prob] : GenInfo->SupportInfo.OriginalVUInfo.VL) {
    auto Name =
        GeneratorFactory<VLGeneratorHolder>::create(GenStr.asStr())->identify();
    if (find(DiscardedVLNames, Name) == DiscardedVLNames.end())
      VLGensNames.emplace_back(Name, Prob);
  }
  assert(!VLGensNames.empty());
  VLGensNames.normalizeProbs();

  OS << "  - VL Selection Rules:\n";
  for (const auto &[Str, Prob] : VLGensNames) {
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5) << " ";
    OS << "<" << Str << ">\n";
  }

  OS << "  - VM Selection Rules:\n";
  for (const auto &[Gen, Prob] :
       zip_equal(VMGen.Items, VMGen.Dist.probabilities())) {
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5) << " ";
    OS << "<" << Gen->identify() << ">\n";
  }

  OS << "  - Configuration Bag Listing:\n";
  unsigned IllegalPointsSize = 0;

  // This ignores VL.
  auto AllPossibleConfigs = jointProbabilityDistribution(
      CfgGen.getCombinedDistribution(getModeChangeInfo())
          .getAllPossibleSewLmulPairs(),
      CfgGen.VmaGen.toProbableItems(), CfgGen.VtaGen.toProbableItems(),
      CfgGen.VxrmGen.toProbableItems());
  AllPossibleConfigs.normalizeProbs();

  for (const auto &[Cfg, Prob] : AllPossibleConfigs) {
    auto [SewLmul, MA, TA, XRM] = Cfg;
    auto [SEW, LMUL] = SewLmul;
    if (!isLegalSewLmul(ELEN, VLEN, SEW, LMUL)) {
      ++IllegalPointsSize;
      continue;
    }
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5);
    OS << " Conf: ";
    OS << "{ " << toString(SEW) << ", " << toString(LMUL) << ", "
       << toString(TA) << ", " << toString(MA) << ", vxrm: " << toString(XRM)
       << " }";
    OS << "/MaxVL: " << computeVLMax(ELEN, VLEN, SEW, LMUL);
    OS << "\n";
  }
  if (IllegalPointsSize > 0) {
    OS << "    ";
    OS << "P: " << floatToString(SwitchInfo.ProbSetVill, 5) << " Conf: ";
    OS << "{  Illegal Configurations:  " << IllegalPointsSize
       << " points }/MaxVL: 0\n";
  }
  OS << "  - Configuration Bag Size: " << AllPossibleConfigs.size() << "\n";

  // This "cardinality" is completely fake and is kept for backward
  // compatibility. The true amount of {VL, SEW, LMUL, MA, TA, XRM} states can
  // be computed as PrimaryConfigsSize * SecondaryConfigsSize, where unsigned
  // PrimaryConfigsSize =
  //     CfgGen.PrimaryGen.getAllPossibleConfigs().size();
  // unsigned SecondaryConfigsSize =
  //     jointProbabilityDistribution(CfgGen.VmaGen.toWeightsArray(),
  //                                  CfgGen.VtaGen.toWeightsArray(),
  //                                  CfgGen.VxrmGen.toWeightsArray()).size();
  unsigned Cardinality = 0;
  for (const auto &[Cfg, Prob] : AllPossibleConfigs) {
    auto [SewLmul, MA, TA, XRM] = Cfg;
    auto [SEW, LMUL] = SewLmul;
    Cardinality += computeVLMax(ELEN, VLEN, SEW, LMUL);
  }
  OS << "  - State Cardinality: " << Cardinality << " ~ {MASKS} \n";
  OS << "--- RVV Configuration End  ---\n\n";
}

RISCVConfigurationInfo
RISCVConfigurationInfo::constructConfiguration(LLVMState &State,
                                               const Config &Cfg) {
  auto &Ctx = State.getCtx();
  const auto &TM = State.getTargetMachine();
  auto ArchInfo =
      RISCVConfigurationInfo::deriveArchitecturalInformation(Ctx, TM);
  auto BaseCfg = BaseConfigurationInfo(ArchInfo.XLEN);
  auto RVVCfg = RVVConfigurationInfo::buildConfiguration(Cfg, ArchInfo.ELEN,
                                                         ArchInfo.VLEN);

  if (DumpDiscardedRVVConfigurations.isSpecified())
    printDiscardedRVVConfigurations(RVVCfg);

  return RISCVConfigurationInfo(std::move(BaseCfg), std::move(RVVCfg));
}

RISCVConfigurationInfo::ArchitecturalInfo
RISCVConfigurationInfo::deriveArchitecturalInformation(
    LLVMContext &Ctx, const TargetMachine &TM) {
  ArchitecturalInfo Result;
  // To properly process llvm target settings we have to create and query
  // RISCVSubtarget which in turn requires an LLVM function...  So we create a
  // temporary module to do the necessary requests without disturbing the
  // primary one.
  Module M("TemporaryModule", Ctx);
  auto *DummyFT = FunctionType::get(Type::getVoidTy(Ctx), false);
  constexpr const char *kDummyFnName = "Dummy";
  M.getOrInsertFunction(kDummyFnName, DummyFT);
  const auto &ST =
      TM.getSubtarget<RISCVSubtarget>(*M.getFunction(kDummyFnName));

  Result.XLEN = ST.getXLen();

  if (ST.hasStdExtV()) {
    Result.ELEN = ST.getELen();
    // This is what's specified in the march with Zvl*b. It usually specifies
    // the *minimum* VLEN, but since snippy targets a specific VLEN we can
    // use that directly. This will be used to configure the models with a
    // proper VLEN too.
    Result.VLEN = ST.getRealMinVLen();
  }

  LLVM_DEBUG({
    dbgs() << formatv("\n=== Derived Architectural Info ===\n"
                      "XLEN={0}, ELEN={1}, VLEN={2}\n\n",
                      Result.XLEN, Result.ELEN, Result.VLEN);
  });

  return Result;
}

} // namespace snippy
} // namespace llvm
