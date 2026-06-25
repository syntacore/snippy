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

// NOTE: RISCV backend can (to an extend) describe RVV configuration limits
// of the target with -riscv-v-vector-bits-max, -riscv-v-vector-bits-min
// llvm options. User has an option to specify Zvl*b extension, which
// affect the list of possible configuations too. For now, llvm-snippy
// does not use these mechanisms since they complicate generator usage.
// We may revise this policy later once the code base is stable enough.
static snippy::opt<bool> UseNonSimplifiedRVVConfig(
    "snippy-riscv-disable-simplified-rvv-configuration",
    cl::desc("Experimental. Extract RVV configuration limits from RISCV "
             "Subtarget instead of generator-specific options."),
    cl::Hidden, cl::init(false), cl::cat(SnippyRISCVOptions));

static snippy::opt<unsigned> SimplifiedRVV_VLEN(
    "snippy-riscv-simplified-vector-bits-max",
    cl::desc("Defines the size of vector register file when simplified "
             "(the default) RVV configuration is active."),
    cl::Hidden, cl::init(128), cl::cat(SnippyRISCVOptions));

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

struct MaxPossibleVLGen final : VLGeneratorInterface {

  static constexpr const char *kID = "max_encodable";
  std::string identify() const override { return kID; }

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

  static constexpr const char *kID = "vlmax";
  std::string identify() const override { return kID; }

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

  static constexpr const char *kID = "any_legal";
  std::string identify() const override { return kID; }

  VLDistributionType getDistribution(unsigned VLMax) const override {
    return VLDistributionType(VLMax + 1, 1.0 / (VLMax + 1));
  }
};

struct LegalVLNonZeroGenerator final : VLGeneratorInterface {

  static constexpr const char *kID = "any_legal_non_zero";
  std::string identify() const override { return kID; }

  VLDistributionType getDistribution(unsigned VLMax) const override {
    VLDistributionType Result(VLMax + 1);
    for (auto &W : drop_begin(Result))
      W = 1.0 / VLMax;
    return Result;
  }
};

struct UnmaskedVMGenerator final : VMGeneratorInterface {

  static constexpr const char *kID = "all_ones";
  std::string identify() const override { return kID; }

  unsigned getMinRequiredVL() const override { return 0; }

  APInt generate(unsigned VL) const override { return APInt::getAllOnes(VL); }
};

struct LegalVMGenerator final : VMGeneratorInterface {

  static constexpr const char *kID = "any_legal";
  std::string identify() const override { return kID; }

  unsigned getMinRequiredVL() const override { return 0; }

  APInt generate(unsigned VL) const override {
    auto MaxValue = APInt::getAllOnes(VL);
    return APInt(RandEngine::genInRangeInclusive(MaxValue));
  }
};

static APInt getImmVLVM(StringRef Item, StringRef ErrorContext) {
  // Here we are trying to convert a string Item from the yaml config
  // to a valid VL or VM value, depending on the context ErrorContext.
  Expected<FormattedAPIntWithSign> ExpectedValue =
      FormattedAPIntWithSign::fromString(Item);
  if (auto E = ExpectedValue.takeError())
    snippy::fatal(Twine("Illegal IMM-based ") + ErrorContext + ": " + Item);

  // If the converted number is negative, it is an error.
  // Non-negative VLs and VMs are expected.
  if (ExpectedValue->Number.IsSigned)
    snippy::fatal(Twine(ErrorContext) + " can't be negative: " + Item);
  return ExpectedValue->Number.Value;
}

struct ImmVLGen : public VLGeneratorInterface {

  ImmVLGen(std::string ID) {
    auto APIntVal = getImmVLVM(ID, "VL");
    Context = std::string(kID) + "_" + ID;
    if (APIntVal.getActiveBits() > sizeof(Value) * CHAR_BIT)
      snippy::fatal(Twine("VL ") + Context +
                    std::string(" is greater than the maximum possible: ") +
                    std::to_string(sizeof(Value) * CHAR_BIT));
    Value = APIntVal.getZExtValue();
  }

  static constexpr const char *kID = "imm";
  std::string identify() const override { return Context; }

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

  ImmVMGen(std::string ID) {
    Value = getImmVLVM(ID, "VM");
    Context = std::string(ImmVLGen::kID) + "_" + ID;
  }

  std::string identify() const override { return Context; }

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

template <typename T>
constexpr bool compareTypeIdWithString(std::string_view S) {
  return std::string_view(T::kID) == S;
}
template <typename... U>
constexpr bool compareIdFromTypesWithString(std::string_view S) {
  return (compareTypeIdWithString<U>(S) || ...);
}

template <typename T> constexpr bool hasDuplicateId() { return false; }

template <typename T, typename X, typename... U>
constexpr bool hasDuplicateId() {
  return compareIdFromTypesWithString<X, U...>(T::kID) ||
         hasDuplicateId<X, U...>();
}

template <typename ResultType, typename Default>
static std::unique_ptr<ResultType> constructByID(const std::string_view &ID) {
  return std::make_unique<Default>(std::string(ID.begin(), ID.end()));
}

template <typename ResultType, typename Default, typename T, typename... U>
static std::unique_ptr<ResultType> constructByID(const std::string_view &ID) {
  static_assert(!hasDuplicateId<T, U...>());
  if (T::kID == ID)
    return std::make_unique<T>();
  return constructByID<ResultType, Default, U...>(ID);
}

template <typename Result> struct GeneratorFactory;

template <> struct GeneratorFactory<VLGeneratorHolder> {
  using ObjectType = VLGeneratorInterface;
  static VLGeneratorHolder create(const std::string &ID) {
    return constructByID<VLGeneratorInterface, ImmVLGen, MaxPossibleVLGen,
                         MaxVLGenerator, LegalVLGenerator,
                         LegalVLNonZeroGenerator>(ID);
  }
};
template <> struct GeneratorFactory<VMGeneratorHolder> {
  using ObjectType = VMGeneratorInterface;
  static VMGeneratorHolder create(const std::string &ID) {
    return constructByID<VMGeneratorInterface, ImmVMGen, UnmaskedVMGenerator,
                         LegalVMGenerator>(ID);
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
}

static std::string toString(VLMUL LMUL) {
  switch (LMUL) {
  case VLMUL::LMUL_RESERVED:
    return "reserved";
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
  //      | reserved | mf8 | mf4 | mf2 |  m1 |  m2 |  m4 |  m8
  //    8 |      0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  //   16 |      0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  //  . . . . .
  //  512 |      0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5
  // 1024 |      0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5

  constexpr std::array ColumnsWidths = {9, 5, 5, 5, 5, 5, 5, 5};
  constexpr std::array LMULs = {
      VLMUL::LMUL_RESERVED, VLMUL::LMUL_F8, VLMUL::LMUL_F4, VLMUL::LMUL_F2,
      VLMUL::LMUL_1,        VLMUL::LMUL_2,  VLMUL::LMUL_4,  VLMUL::LMUL_8};
  constexpr auto SewWidth = 9;

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
                            const SEWInfo &SEWWeights,
                            const LMULInfo &LMULWeights, double PVill) {
  auto SEWProbs = normalizeWeights(SEWWeights);
  auto LMULProbs = normalizeWeights(LMULWeights);
  LLVM_DEBUG(printRawSewLmulProbs(dbgs(), SEWProbs, LMULProbs));

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

  auto IsLegal =
      [=](const ProbableElement<std::tuple<VSEW, VLMUL>> &Item) -> bool {
    auto [SEW, LMUL] = Item.Element;
    return isLegalSewLmul(ELEN, VLEN, SEW, LMUL);
  };
  auto LegalRange = make_filter_range(Dist, IsLegal);
  auto IllegalRange = make_filter_range(Dist, std::not_fn(IsLegal));

  // Note that at the moment users can't set probabilities for reserved
  // values of SEW and LMUL, so we copy only probs for legal values
  // and ignore the rest. All illegal configs will have the same probability.
  for (auto &[SewLmul, P] : LegalRange) {
    auto [SEW, LMUL] = SewLmul;
    P = SEWProbs[SEW] * LMULProbs[LMUL];
  }

  // The total probability of all illegal combinations must be PVill. The total
  // probability of all legal combinations must be 1 - PVill.
  auto AddProb = [](double Acc,
                    const ProbableElement<std::tuple<VSEW, VLMUL>> &E) {
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
    IO.mapRequired("SEW", VTYPE.SEW);
    IO.mapRequired("LMUL", VTYPE.LMUL);
    IO.mapRequired("VMA", VTYPE.VMA);
    IO.mapRequired("VTA", VTYPE.VTA);
  }
};

template <> struct snippy::YAMLHistogramTraits<VLVMSequence::VLVMEntry> {
  using DenormEntry = VLVMSequence::VLVMEntry;
  using MapType = VLVMSequence;

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef ParseStr,
                                      double Weight) {
    return {ParseStr.data(), Weight};
  }

  static void normalizeEntry(yaml::IO &Io, const DenormEntry &E,
                             SmallVectorImpl<SValue> &RawStrings) {
    RawStrings.push_back(E.first);
    RawStrings.push_back(std::to_string(E.second));
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> VLVMs) {
    return {VLVMs};
  }

  static void normalizeMap(yaml::IO &Io, const MapType &Entries,
                           std::vector<DenormEntry> &VLVMs) {
    VLVMs = Entries.Values;
  }

  static std::string validate(ArrayRef<DenormEntry> VLVMs) {
    return checkWeights(make_second_range(VLVMs), "VL/VM");
  }
};

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(VLVMSequence);
LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(VLVMSequence::VLVMEntry)

void yaml::MappingTraits<VLVMSequence>::mapping(yaml::IO &IO,
                                                VLVMSequence &VLVMs) {
  EmptyContext Ctx;
  yaml::yamlize(IO, VLVMs.Values, false, Ctx);
}

std::string yaml::MappingTraits<VLVMSequence>::validate(yaml::IO &,
                                                        VLVMSequence &VLVMs) {
  return YAMLHistogramTraits<VLVMSequence::VLVMEntry>::validate(VLVMs.Values);
}

template <> struct yaml::MappingTraits<RVVUnitInfo> {
  static void mapping(yaml::IO &IO, RVVUnitInfo &VUInfo) {
    IO.mapRequired("VXRM", VUInfo.VXRM);
    IO.mapRequired("VTYPE", VUInfo.VTYPE);

    IO.mapOptional("VM", VUInfo.VM);
    IO.mapOptional("VL", VUInfo.VL);
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

namespace snippy {

static std::vector<double>
buildWeights(unsigned ELEN, unsigned VLEN, const PrimaryConfigMapping &Mapping,
             const SewLmulDistribution &SewLmulDist,
             const ProbableItems<VLGeneratorHolder> &VLGenerators,
             bool IsForVSETIVLI) {
  // Weights for each combination of {SEW, LMUL, VL}.
  std::vector<double> Weights(
      Mapping.SEWSize * Mapping.LMULSize * Mapping.VLSize, 0.0);

  // Construct VL distributions for each combination of {SEW, LMUL}
  VlDistributionStorage VLDistStorage(ELEN, VLEN, VLGenerators, IsForVSETIVLI);

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

RVVPrimaryConfigGenerator::RVVPrimaryConfigGenerator(
    unsigned ELEN, unsigned VLEN, const SewLmulDistribution &SewLmulDist,
    const ProbableItems<VLGeneratorHolder> &VLGenerators,
    unsigned MinVMBitWidth, bool IsForVSETIVLI)
    : VLSize(IsForVSETIVLI ? (kMaxVLForVSETIVLI + 1)
                           : (getMaxPossibleVL(ELEN, VLEN) + 1)),
      IsForVSETIVLI(IsForVSETIVLI), Mapping(VLSize) {

  auto Weights = buildWeights(ELEN, VLEN, Mapping, SewLmulDist, VLGenerators,
                              IsForVSETIVLI);

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
  constexpr unsigned SewWidth = 9;
  constexpr unsigned LmulWidth = 9;
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
  assert(!MustUseReducedVL ||
         PrimaryGenReduced &&
             "Requested to sample a mode for VSETIVLI but there "
             "is no generator for it");
  assert(MustUseReducedVL ||
         PrimaryGen && "Requested to sample a mode for VSETVLI or VSETVL but "
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

std::pair<unsigned, bool> computeDecodedEMUL(unsigned ELEN, unsigned SEW,
                                             unsigned EEW, VLMUL LMUL) {
  if (!isLegalSewLmul(ELEN, EEW, VSEW(SEW), LMUL)) {
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

  auto SEWNorm = normalizeWeights(VUInfo.VTYPE.SEW);
  auto LMULNorm = normalizeWeights(VUInfo.VTYPE.LMUL);
  const auto &RawSewLmulPairs = jointProbabilityDistribution(SEWNorm, LMULNorm);

  auto MANorm = normalizeWeights(VUInfo.VTYPE.VMA);
  auto TANorm = normalizeWeights(VUInfo.VTYPE.VTA);
  auto XRMNorm = normalizeWeights(VUInfo.VXRM);
  const auto &SecondaryCombos =
      jointProbabilityDistribution(MANorm, TANorm, XRMNorm);

  std::vector<std::tuple<VSEW, VLMUL, VTAMode, VMAMode, VXRMMode>> Result;
  for (const auto &[SewLmul, Prob] : RawSewLmulPairs) {
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
  VLVMSequence VMSeq = {{{UnmaskedVMGenerator::kID, 1.0}}};
  VLVMSequence VLSeq = {{{MaxPossibleVLGen::kID, 1.0}}};

  VTypeInfo VTYPE{SEW, LMUL, VMA, VTA};
  RVVUnitInfo VUInfo{VXRM, VTYPE, VMSeq, VLSeq};
  RVVConfigurationSpace CS{
      /*no bias, deduce P from histogram*/ ModeChangeBias{}, VUInfo};

  return CS;
}

// MinVL = minimum bit width of all VM generators
static unsigned getMinRequestedBitWidth(const VLVMSequence &VMSeq) {
  assert(!VMSeq.Values.empty());
  unsigned MinVL = std::numeric_limits<unsigned>::max();
  for (const auto &[Str, Weight] : VMSeq.Values) {
    auto VMGen = GeneratorFactory<VMGeneratorHolder>::create(Str);
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
                                    const VLVMSequence &OriginalVLSeq,
                                    const VLVMSequence &OriginalVMSeq,
                                    const SewLmulDistribution &SewLmulDist,
                                    bool IsOnlyVSETIVLI) {
  VLVMInfo Result;

  // We discard all VL generators for which there is no available VM
  // generator (MinBitWidth > MaxVL of this generator).
  unsigned MinRequestedBitWidth = getMinRequestedBitWidth(OriginalVMSeq);

  unsigned GlobalMaxVL = 0;
  for (const auto &[Str, Weight] : OriginalVLSeq.Values) {
    auto VLGen = GeneratorFactory<VLGeneratorHolder>::create(Str);
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
  for (const auto &[Str, Weight] : OriginalVMSeq.Values) {
    auto VMGen = GeneratorFactory<VMGeneratorHolder>::create(Str);
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

  const auto &SewLmulDist =
      buildRawSewLmulDistribution(ELEN, VLEN, CS.VUInfo.VTYPE.SEW,
                                  CS.VUInfo.VTYPE.LMUL, SwitchInfo.ProbSetVill);

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

  unsigned MinVMBitWidth = getMinRequestedBitWidth(CS.VUInfo.VM);
  // One generator for (VSETVLI & VSETVL) and another for VSETIVLI

  std::optional<RVVPrimaryConfigGenerator> PrimaryConfigGen;
  if (IsPresentNonVSETIVLI)
    PrimaryConfigGen.emplace(ELEN, VLEN, SewLmulDist, VLGenerators,
                             MinVMBitWidth,
                             /*BuildForVSETIVLI*/ false);

  std::optional<RVVPrimaryConfigGenerator> PrimaryConfigGenReduced;
  if (IsPresentVSETIVLI)
    PrimaryConfigGenReduced.emplace(ELEN, VLEN, SewLmulDist, VLGenerators,
                                    MinVMBitWidth,
                                    /*BuildForVSETIVLI*/ true);

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
  for (const auto &[GenStr, Prob] :
       GenInfo->SupportInfo.OriginalVUInfo.VL.Values) {
    auto Name = GeneratorFactory<VLGeneratorHolder>::create(GenStr)->identify();
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
  Module M("TemporayModule", Ctx);
  auto *DummyFT = FunctionType::get(Type::getVoidTy(Ctx), false);
  constexpr const char *kDummyFnName = "Dummy";
  M.getOrInsertFunction(kDummyFnName, DummyFT);
  const auto &ST =
      TM.getSubtarget<RISCVSubtarget>(*M.getFunction(kDummyFnName));

  Result.XLEN = ST.getXLen();

  if (!ST.hasStdExtV())
    return Result;

  Result.ELEN = ST.getELen();

  if (!UseNonSimplifiedRVVConfig) {
    Result.VLEN = SimplifiedRVV_VLEN;
    return Result;
  }

  Result.VLEN = ST.getRealMaxVLen();
  return Result;
}

} // namespace snippy
} // namespace llvm
