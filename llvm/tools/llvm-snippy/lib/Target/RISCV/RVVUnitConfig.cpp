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

static snippy::opt<bool> ExcludeVLMAXOne(
    "snippy-riscv-rvv-wa-exclude-vlmax1",
    cl::desc("Excludes cases when VLMAX is 1 (happens when LMUL is < 1). "
             "Currently, it seems that our SW models do not handle this case "
             "correctly."),
    cl::Hidden, cl::init(true), cl::cat(SnippyRISCVOptions));

static snippy::opt<bool> NoReservedCfgRVV(
    "riscv-disable-reserved-sew-lmul",
    cl::desc(
        "This option disables the generation of reserved SEW and LMUL values"),
    cl::Hidden, cl::init(false), cl::cat(SnippyRISCVOptions));

} // namespace snippy
} // namespace llvm

namespace {

using namespace llvm;
using namespace llvm::snippy;

template <typename T, typename B>
bool cartesianIncrement(const B &begins, std::pair<T, T> &Range) {
  ++Range.first;
  if (Range.first == Range.second)
    return true;
  return false;
}
template <typename T, typename... TT, typename B>
bool cartesianIncrement(const B &begins, std::pair<T, T> &Range,
                        std::pair<TT, TT> &...Tail) {
  ++Range.first;
  if (Range.first == Range.second) {
    Range.first =
        std::get<std::tuple_size<B>::value - sizeof...(Tail) - 1>(begins);
    return cartesianIncrement(begins, Tail...);
  }
  return false;
}

template <typename Container> auto cartesianRange(const Container &Cont) {
  return std::make_pair(Cont.begin(), Cont.end());
}
// I dream about C++23
template <typename OutputIterator, typename... Iter>
void cartesianProduct(OutputIterator Output, std::pair<Iter, Iter>... Ranges) {
  const auto begins = std::make_tuple(Ranges.first...);
  for (;;) {
    Output = {*Ranges.first...};
    if (cartesianIncrement(begins, Ranges...))
      break;
  }
}

template <typename EnType> struct WeightsStorage {
  using EnumerationType = EnType;
  using WeightType = double;
  using IndexType = std::underlying_type_t<EnType>;
  using StorageType =
      std::array<WeightType, static_cast<IndexType>(EnType::ItemsNum)>;

  StorageType W;

  WeightType &operator[](EnType idx) { return W[static_cast<IndexType>(idx)]; }

  const WeightType &operator[](EnType idx) const {
    return W[static_cast<IndexType>(idx)];
  }
};

template <typename ItType>
static bool checkWeightsNonNegative(ItType Begin, ItType End) {
  return std::all_of(Begin, End, [](const auto Item) { return Item >= 0; });
}

template <typename ItType>
static bool checkNonZeroWeightPresent(ItType Begin, ItType End) {
  return std::any_of(Begin, End, [](const auto Item) { return Item > 0.0; });
}

template <typename ItType>
void checkWeights(ItType Begin, ItType End, const llvm::Twine &What) {
  if (!checkWeightsNonNegative(Begin, End))
    snippy::fatal(What + ": weights must be non-negative!");

  if (!checkNonZeroWeightPresent(Begin, End))
    snippy::fatal(What + ": at least one weight must be positive!");
}

template <typename ItType>
static void dumpRawPropabilities(llvm::raw_ostream &Stream,
                                 llvm::StringRef What, ItType Begin,
                                 ItType End) {
  Stream << "Raw Propabilities: <" << What << ">:";
  for (auto It = Begin; It != End; ++It) {
    Stream << " " << It->Value << "{" << floatToString(It->P, 5 /*precision*/)
           << "}";
  }
  Stream << "\n";
}

enum class LMULTypes : unsigned {
  M1,
  M2,
  M4,
  M8,
  MF2,
  MF4,
  MF8,
  MReserved,
  ItemsNum
};
enum class SEWTypes : unsigned {
  SEW8,
  SEW16,
  SEW32,
  SEW64,
  SEWReserved1,
  SEWReserved2,
  SEWReserved3,
  SEWReserved4,
  ItemsNum
};
enum class VXRMTypes : unsigned { RNU, RNE, RDN, RON, ItemsNum };
enum class VMAMode : unsigned { MU, MA, ItemsNum };
enum class VTAMode : unsigned { TU, TA, ItemsNum };
enum class VXSATModes : unsigned { OFF, ON, ItemsNum };

using VXRMInfo = WeightsStorage<VXRMTypes>;
using VXSATInfo = WeightsStorage<VXSATModes>;
using SEWInfo = WeightsStorage<SEWTypes>;
using LMULInfo = WeightsStorage<LMULTypes>;
using VMAInfo = WeightsStorage<VMAMode>;
using VTAInfo = WeightsStorage<VTAMode>;

struct BiasGuides {
  bool Enabled = false;
  double ModeChangeP = 0.0;
  double SetVillP = 0.0;
};

struct VTypeInfo {
  SEWInfo SEW;
  LMULInfo LMUL;
  VMAInfo VMA;
  VTAInfo VTA;
};

struct RVVUnitInfo {
  VXRMInfo VXRM;
  VXSATInfo VXSAT;
  VTypeInfo VTYPE;

  std::vector<SList> VM;
  std::vector<SList> VL;
};

struct RVVConfigurationSpace {
  BiasGuides Guides;
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

struct ConfigPoint {
  ConfigurationElement<SEWTypes> SEW;
  ConfigurationElement<LMULTypes> LMUL;
  ConfigurationElement<VMAMode> VMA;
  ConfigurationElement<VTAMode> VTA;
  ConfigurationElement<VXRMTypes> VXRM;
  ConfigurationElement<VXSATModes> VXSAT;
};

template <typename SliceType>
static auto extractElementsWithPropabilities(const SliceType &ConfSlice) {
  using EnumerationType = typename SliceType::EnumerationType;
  using Element = ConfigurationElement<EnumerationType>;
  std::vector<Element> Result;
  // First we need to normalize weights to get propabilities
  auto WeightSum = std::accumulate(ConfSlice.W.begin(), ConfSlice.W.end(), 0.0);
  constexpr auto NumberOfItems = static_cast<size_t>(EnumerationType::ItemsNum);
  static_assert(NumberOfItems ==
                std::tuple_size_v<typename SliceType::StorageType>);
  using IndexType = typename Element::UnderlyingType;
  for (IndexType Idx = 0; Idx < NumberOfItems; ++Idx) {
    auto Weight = ConfSlice.W[Idx];
    if (Weight > 0.0)
      Result.push_back({Weight / WeightSum, Idx});
  }
  assert(std::abs(std::accumulate(Result.begin(), Result.end(), 0.0,
                                  [](const auto &Acc, const auto &Item) {
                                    return Acc + Item.P;
                                  }) -
                  1.0) < kProbabilityThreshold);
  return Result;
}

struct RVVModeSwitchingInfo {
  bool RVVPresentInHistogram;
  bool VSETPresentInHistogram;
  ModeChangeInfo SwitchInfo;
};

static ModeChangeInfo createModeChangeInfoForDisabledRVV() {
  ModeChangeInfo Result;
  Result.RVVPresent = false;

  Result.ProbSetVill = 0.0;

  Result.ProbVSETVL = 0.0;
  Result.ProbVSETVLI = 0.0;
  Result.ProbVSETIVLI = 0.0;

  // NOTE: we still need weights to be defined for potential initialization of
  // vector registers
  Result.WeightVSETVL = 1.0;
  Result.WeightVSETVLI = 1.0;
  Result.WeightVSETIVLI = 1.0;

  return Result;
}

static ModeChangeInfo
createModeChangeInfoForHistogramMode(double WeightOfAllRVVInstructions,
                                     double ProbVSETVL, double ProbVSETVLI,
                                     double ProbVSETIVLI, double ProbVill) {
  ModeChangeInfo Result;

  Result.RVVPresent = true;

  Result.ProbSetVill = ProbVill;

  Result.ProbVSETVL = ProbVSETVL;
  Result.ProbVSETVLI = ProbVSETVLI;
  Result.ProbVSETIVLI = ProbVSETIVLI;

  std::discrete_distribution<int> D = {ProbVSETVL, ProbVSETVLI, ProbVSETIVLI};
  auto Prob = D.probabilities();
  // We scale weights proportionally to the relative weight of each
  // mode-changing instruction
  Result.WeightVSETVL = WeightOfAllRVVInstructions * Prob[0];
  Result.WeightVSETVLI = WeightOfAllRVVInstructions * Prob[1];
  Result.WeightVSETIVLI = WeightOfAllRVVInstructions * Prob[2];
  return Result;
}

static ModeChangeInfo
createModeChangeInfoBiasedMode(double WeightOfAllInstructions,
                               double RVVConfigBias, double ProbVill) {
  ModeChangeInfo Result;
  Result.RVVPresent = true;

  auto ModeChangeWeight = WeightOfAllInstructions * RVVConfigBias;
  auto ModeChangeProbability =
      ModeChangeWeight / (WeightOfAllInstructions + ModeChangeWeight);

  Result.ProbSetVill = ProbVill;

  Result.ProbVSETVL = ModeChangeProbability / 3.0;
  Result.ProbVSETVLI = ModeChangeProbability / 3.0;
  Result.ProbVSETIVLI = ModeChangeProbability / 3.0;

  Result.WeightVSETVL = ModeChangeWeight / 3.0;
  Result.WeightVSETVLI = ModeChangeWeight / 3.0;
  Result.WeightVSETIVLI = ModeChangeWeight / 3.0;

  return Result;
}

RVVModeSwitchingInfo deriveModeSwitchingProbability(const Config &Cfg,
                                                    double ConfigurationBias,
                                                    double ProbSetVill) {
  auto OpcGen = Cfg.createDefaultOpcodeGenerator();
  auto ProbInfo = OpcGen->getProbabilities();
  bool RVVInstructionsFound =
      std::any_of(ProbInfo.begin(), ProbInfo.end(), [](const auto &Item) {
        static_assert(std::is_same_v<decltype(Item.first), const unsigned>);
        static_assert(std::is_same_v<decltype(Item.second), double>);
        return isRVV(Item.first) && (Item.second > 0.0);
      });
  bool VSETPInstructionsFound =
      std::any_of(ProbInfo.begin(), ProbInfo.end(), [](const auto &Item) {
        static_assert(std::is_same_v<decltype(Item.first), const unsigned>);
        static_assert(std::is_same_v<decltype(Item.second), double>);
        return isRVVModeSwitch(Item.first) && (Item.second > 0.0);
      });

  constexpr bool RVVPresentInHistogram = true;
  constexpr bool RVVMissingInHistogram = false;

  constexpr bool VSETPresentInHistogram = true;
  constexpr bool VSETMissingInHistorgram = false;

  if (!RVVInstructionsFound)
    return {RVVMissingInHistogram, VSETMissingInHistorgram,
            createModeChangeInfoForDisabledRVV()};

  if (!VSETPInstructionsFound)
    return {RVVPresentInHistogram, VSETMissingInHistorgram,
            createModeChangeInfoBiasedMode(Cfg.Histogram.getTotalWeight(),
                                           ConfigurationBias, ProbSetVill)};

  double RVVWeight = Cfg.Histogram.getOpcodesWeight(
      [](unsigned Opcode) { return isRVV(Opcode); });
  return {RVVPresentInHistogram, VSETPresentInHistogram,
          createModeChangeInfoForHistogramMode(
              RVVWeight, ProbInfo[RISCV::VSETVL], ProbInfo[RISCV::VSETVLI],
              ProbInfo[RISCV::VSETIVLI], ProbSetVill)};
}

static auto convertLMULRepresentation(unsigned LMULInternal) {
  assert(LMULInternal < static_cast<unsigned>(LMULTypes::ItemsNum));
  switch (static_cast<LMULTypes>(LMULInternal)) {
  case LMULTypes::M1:
    return RISCVII::VLMUL::LMUL_1;
  case LMULTypes::M2:
    return RISCVII::VLMUL::LMUL_2;
  case LMULTypes::M4:
    return RISCVII::VLMUL::LMUL_4;
  case LMULTypes::M8:
    return RISCVII::VLMUL::LMUL_8;
  case LMULTypes::MReserved:
    return RISCVII::VLMUL::LMUL_RESERVED;
  case LMULTypes::MF2:
    return RISCVII::VLMUL::LMUL_F2;
  case LMULTypes::MF4:
    return RISCVII::VLMUL::LMUL_F4;
  case LMULTypes::MF8:
    return RISCVII::VLMUL::LMUL_F8;
  default:
    llvm_unreachable("incorrect LMULInternal representation");
  }
}

static auto convertSEWRepresentation(unsigned SEWInternal) {
  assert(SEWInternal < static_cast<unsigned>(SEWTypes::ItemsNum));
  switch (static_cast<SEWTypes>(SEWInternal)) {
  case SEWTypes::SEW8:
    return snippy::RVVConfiguration::VSEW::SEW8;
  case SEWTypes::SEW16:
    return snippy::RVVConfiguration::VSEW::SEW16;
  case SEWTypes::SEW32:
    return snippy::RVVConfiguration::VSEW::SEW32;
  case SEWTypes::SEW64:
    return snippy::RVVConfiguration::VSEW::SEW64;
  case SEWTypes::SEWReserved1:
    return snippy::RVVConfiguration::VSEW::SEWReserved1;
  case SEWTypes::SEWReserved2:
    return snippy::RVVConfiguration::VSEW::SEWReserved2;
  case SEWTypes::SEWReserved3:
    return snippy::RVVConfiguration::VSEW::SEWReserved3;
  case SEWTypes::SEWReserved4:
    return snippy::RVVConfiguration::VSEW::SEWReserved4;
  default:
    llvm_unreachable("incorrect SEWInternal representation");
  }
}

static auto convertVXRMRepresentation(unsigned VXRMInternal) {
  switch (static_cast<VXRMTypes>(VXRMInternal)) {
  case VXRMTypes::RNU:
    return snippy::RVVConfiguration::VXRMMode::RNU;
  case VXRMTypes::RNE:
    return snippy::RVVConfiguration::VXRMMode::RNE;
  case VXRMTypes::RDN:
    return snippy::RVVConfiguration::VXRMMode::RDN;
  case VXRMTypes::RON:
    return snippy::RVVConfiguration::VXRMMode::RON;
  default:
    llvm_unreachable("incorrect VXRMInternal representation");
  }
}

static auto convertVXSatRepresentation(unsigned VXSatInternal) {
  switch (static_cast<VXSATModes>(VXSatInternal)) {
  case VXSATModes::OFF:
    return false;
  case VXSATModes::ON:
    return true;
  default:
    llvm_unreachable("incorrect TAInternal representation");
  }
}

static auto convertMARepresentation(unsigned MAInternal) {
  switch (static_cast<VMAMode>(MAInternal)) {
  case VMAMode::MU:
    return false;
  case VMAMode::MA:
    return true;
  default:
    llvm_unreachable("incorrect TAInternal representation");
  }
}

static auto convertTARepresentation(unsigned TAInternal) {
  switch (static_cast<VTAMode>(TAInternal)) {
  case VTAMode::TU:
    return false;
  case VTAMode::TA:
    return true;
  default:
    llvm_unreachable("incorrect TAInternal representation");
  }
}

struct InternalConfigurationPoint {
  double Probability;
  RVVConfiguration Config;
};

static auto convertRepresentation(unsigned VLEN, const ConfigPoint &Point) {
  InternalConfigurationPoint Result;
  Result.Probability = Point.SEW.P * Point.LMUL.P * Point.VMA.P * Point.VTA.P *
                       Point.VXRM.P * Point.VXSAT.P;
  Result.Config.LMUL = convertLMULRepresentation(Point.LMUL.Value);
  Result.Config.SEW = convertSEWRepresentation(Point.SEW.Value);
  Result.Config.VXRM = convertVXRMRepresentation(Point.VXRM.Value);
  Result.Config.VxsatEnable = convertVXSatRepresentation(Point.VXSAT.Value);
  Result.Config.MaskAgnostic = convertMARepresentation(Point.VMA.Value);
  Result.Config.TailAgnostic = convertTARepresentation(Point.VTA.Value);

  auto MaxVL = computeVLMax(VLEN, static_cast<unsigned>(Result.Config.SEW),
                            Result.Config.LMUL);
  if (MaxVL == 0)
    Result.Config.IsLegal = false;
  return Result;
}

static unsigned getMaxPossibleVL(unsigned VLEN) {
  return VLEN * RVVConfiguration::getMaxLMUL() / RVVConfiguration::getMinSEW();
}

struct MaxVLGenerator final : VLGeneratorInterface {

  static constexpr const char *kID = "vlmax";
  std::string identify() const override { return kID; }

  unsigned generate(unsigned VLEN, const RVVConfiguration &Cfg) const override {
    auto PointSEW = static_cast<unsigned>(Cfg.SEW);
    auto MaxVL = computeVLMax(VLEN, PointSEW, Cfg.LMUL);
    if (MaxVL > 0)
      return MaxVL;
    // If MaxVL == 0 this means that RVVConfiguration is illegal
    // and we just return Max Possible VL
    return getMaxPossibleVL(VLEN);
  }
};

struct LegalVLGenerator final : VLGeneratorInterface {

  static constexpr const char *kID = "any_legal";
  std::string identify() const override { return kID; }

  unsigned generate(unsigned VLEN, const RVVConfiguration &Cfg) const override {
    auto PointSEW = static_cast<unsigned>(Cfg.SEW);
    auto MaxVL = computeVLMax(VLEN, PointSEW, Cfg.LMUL);
    if (MaxVL > 0)
      return RandEngine::genInInterval<unsigned>(0u, MaxVL);
    // If MaxVL == 0 this means that RVVConfiguration is illegal
    // and we just return VL from [0, Max Possible VL]
    return RandEngine::genInInterval<unsigned>(0u, getMaxPossibleVL(VLEN));
  }
};

struct LegalVLNonZeroGenerator final : VLGeneratorInterface {

  static constexpr const char *kID = "any_legal_non_zero";
  std::string identify() const override { return kID; }

  unsigned generate(unsigned VLEN, const RVVConfiguration &Cfg) const override {
    auto PointSEW = static_cast<unsigned>(Cfg.SEW);
    auto MaxVL = computeVLMax(VLEN, PointSEW, Cfg.LMUL);
    if (MaxVL > 0)
      return RandEngine::genInInterval<unsigned>(1u, MaxVL);
    // If MaxVL == 0 this means that RVVConfiguration is illegal
    // and we just return VL from [1, Max Possible VL]
    return RandEngine::genInInterval<unsigned>(1u, getMaxPossibleVL(VLEN));
  }
};

struct UnmaskedVMGenerator final : VMGeneratorInterface {

  static constexpr const char *kID = "all_ones";
  std::string identify() const override { return kID; }

  APInt generate(const RVVConfiguration &Cfg, unsigned VL) const override {
    return APInt::getAllOnes(VL);
  }
};

struct LegalVMGenerator final : VMGeneratorInterface {

  static constexpr const char *kID = "any_legal";
  std::string identify() const override { return kID; }

  APInt generate(const RVVConfiguration &Cfg, unsigned VL) const override {
    auto MaxValue = APInt::getAllOnes(VL);
    return APInt(RandEngine::genInInterval(MaxValue));
  }
};

struct ImmVLGen : public VLGeneratorInterface {

  ImmVLGen(std::string ID) : Context(std::move(ID)) {
    snippy::fatal("IMM-based VL generators are not implemented");
  }

  std::string identify() const override { return "imm_" + Context; }
  unsigned generate(unsigned VLEN, const RVVConfiguration &Cfg) const override {
    return Value;
  }

private:
  unsigned Value;
  std::string Context;
};

struct ImmVMGen : public VMGeneratorInterface {

  ImmVMGen(std::string ID) : Context(std::move(ID)) {
    snippy::fatal("IMM-based VM generators are not implemented");
  }
  std::string identify() const override { return "imm_" + Context; }

  APInt generate(const RVVConfiguration &Cfg, unsigned VL) const override {
    // FIXME: detect illegal combinations
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
template <> struct GeneratorFactory<RVVConfigurationInfo::VLGeneratorHolder> {
  using ObjectType = VLGeneratorInterface;
  static RVVConfigurationInfo::VLGeneratorHolder create(const std::string &ID) {
    return constructByID<VLGeneratorInterface, ImmVLGen, MaxVLGenerator,
                         LegalVLGenerator, LegalVLNonZeroGenerator>(ID);
  }
};
template <> struct GeneratorFactory<RVVConfigurationInfo::VMGeneratorHolder> {
  using ObjectType = VMGeneratorInterface;
  static RVVConfigurationInfo::VMGeneratorHolder create(const std::string &ID) {
    return constructByID<VMGeneratorInterface, ImmVMGen, UnmaskedVMGenerator,
                         LegalVMGenerator>(ID);
  }
};

} // namespace

void RVVConfigurationSpace::mapYaml(llvm::yaml::IO &IO,
                                    std::optional<RVVConfigurationSpace> &CS) {
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
    IO.mapOptional("rnu", VXRM[VXRMTypes::RNU], 0.0);
    IO.mapOptional("rne", VXRM[VXRMTypes::RNE], 0.0);
    IO.mapOptional("rdn", VXRM[VXRMTypes::RDN], 0.0);
    IO.mapOptional("ron", VXRM[VXRMTypes::RON], 0.0);

    checkWeights(VXRM.W.begin(), VXRM.W.end(), "VXRM");
  }
};

template <> struct yaml::MappingTraits<VXSATInfo> {
  static void mapping(yaml::IO &IO, VXSATInfo &VXSAT) {
    IO.mapOptional("on", VXSAT[VXSATModes::ON], 0.0);
    IO.mapOptional("off", VXSAT[VXSATModes::OFF], 0.0);

    checkWeights(VXSAT.W.begin(), VXSAT.W.end(), "VXSAT");
  }
};

template <> struct yaml::MappingTraits<SEWInfo> {
  static void mapping(yaml::IO &IO, SEWInfo &SEW) {
    IO.mapOptional("sew_8", SEW[SEWTypes::SEW8], 0.0);
    IO.mapOptional("sew_16", SEW[SEWTypes::SEW16], 0.0);
    IO.mapOptional("sew_32", SEW[SEWTypes::SEW32], 0.0);
    IO.mapOptional("sew_64", SEW[SEWTypes::SEW64], 0.0);

    checkWeights(SEW.W.begin(), SEW.W.end(), "SEW");
  }
};

template <> struct yaml::MappingTraits<LMULInfo> {
  static void mapping(yaml::IO &IO, LMULInfo &LMUL) {
    IO.mapOptional("m1", LMUL[LMULTypes::M1], 0.0);
    IO.mapOptional("m2", LMUL[LMULTypes::M2], 0.0);
    IO.mapOptional("m4", LMUL[LMULTypes::M4], 0.0);
    IO.mapOptional("m8", LMUL[LMULTypes::M8], 0.0);
    IO.mapOptional("mf2", LMUL[LMULTypes::MF2], 0.0);
    IO.mapOptional("mf4", LMUL[LMULTypes::MF4], 0.0);
    IO.mapOptional("mf8", LMUL[LMULTypes::MF8], 0.0);

    checkWeights(LMUL.W.begin(), LMUL.W.end(), "LMUL");
  }
};

template <> struct yaml::MappingTraits<VMAInfo> {
  static void mapping(yaml::IO &IO, VMAInfo &VMA) {
    IO.mapOptional("mu", VMA[VMAMode::MU], 0.0);
    IO.mapOptional("ma", VMA[VMAMode::MA], 0.0);

    checkWeights(VMA.W.begin(), VMA.W.end(), "VMA");
  }
};

template <> struct yaml::MappingTraits<VTAInfo> {
  static void mapping(yaml::IO &IO, VTAInfo &VTA) {
    IO.mapOptional("tu", VTA[VTAMode::TU], 0.0);
    IO.mapOptional("ta", VTA[VTAMode::TA], 0.0);

    checkWeights(VTA.W.begin(), VTA.W.end(), "VTA");
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

template <> struct yaml::MappingTraits<RVVUnitInfo> {
  static void mapping(yaml::IO &IO, RVVUnitInfo &VUInfo) {
    IO.mapRequired("VXRM", VUInfo.VXRM);
    IO.mapRequired("VXSAT", VUInfo.VXSAT);
    IO.mapRequired("VTYPE", VUInfo.VTYPE);

    IO.mapOptional("VM", VUInfo.VM);
    IO.mapOptional("VL", VUInfo.VL);
  }
};

static bool isCorrectProbability(double Prob) {
  return Prob >= 0.0 && Prob <= 1.0;
}

template <> struct yaml::MappingTraits<BiasGuides> {
  static constexpr auto kProbBounds = "probability should be from [0.0;1.0]";

  static void mapping(yaml::IO &IO, BiasGuides &Guides) {
    Guides.Enabled = true;
    IO.mapRequired("P", Guides.ModeChangeP);
    IO.mapOptional("Pvill", Guides.SetVillP);
  }

  static std::string validate(yaml::IO &IO, BiasGuides &Guides) {
    // TODO: implemenent alternative mode changing schemes and
    // replace probability with weight
    if (!isCorrectProbability(Guides.ModeChangeP))
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

std::unique_ptr<RVVConfigInterface> createRVVConfig() {
  return std::make_unique<RVVConfig>();
}

inline static bool isReservedValues(unsigned SEW, RISCVII::VLMUL LMUL) {
  return LMUL == RISCVII::VLMUL::LMUL_RESERVED || !isLegalSEW(SEW);
}

unsigned computeVLMax(unsigned VLEN, unsigned SEW, RISCVII::VLMUL LMUL) {
  if (isReservedValues(SEW, LMUL))
    return 0;
  assert(canBeEncoded(SEW));
  auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
  if (IsFractional) {
    auto Result = VLEN / SEW / Multiplier;
    if ((Result == 1) && ExcludeVLMAXOne)
      Result = 0;
    return Result;
  }
  return VLEN / SEW * Multiplier;
}

std::pair<unsigned, bool> computeDecodedEMUL(unsigned SEW, unsigned EEW,
                                             RISCVII::VLMUL LMUL) {
  if (isReservedValues(SEW, LMUL) || !isLegalSEW(SEW) || !isLegalSEW(EEW)) {
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

bool isValidEMUL(unsigned SEW, unsigned EEW, RISCVII::VLMUL LMUL) {
  auto [EMUL, IsFractional] = computeDecodedEMUL(SEW, EEW, LMUL);
  return RISCVVType::isValidLMUL(EMUL, IsFractional);
}

RISCVII::VLMUL computeEMUL(unsigned SEW, unsigned EEW, RISCVII::VLMUL LMUL) {
  auto [EMUL, IsFractional] = computeDecodedEMUL(SEW, EEW, LMUL);
  assert(RISCVVType::isValidLMUL(EMUL, IsFractional));
  return RISCVVType::encodeLMUL(EMUL, IsFractional);
}

static unsigned getNumReservedSEW(unsigned SEW) {
  auto SEWEnum = static_cast<RVVConfiguration::VSEW>(SEW);
  switch (SEWEnum) {
  default:
    return 0;
  case RVVConfiguration::VSEW::SEWReserved1:
    return 1;
  case RVVConfiguration::VSEW::SEWReserved2:
    return 2;
  case RVVConfiguration::VSEW::SEWReserved3:
    return 3;
  case RVVConfiguration::VSEW::SEWReserved4:
    return 4;
  }
}

static void printVType(unsigned VType, raw_ostream &OS) {
  unsigned Sew = RISCVVType::getSEW(VType);
  OS << "e";
  if (!isLegalSEW(Sew))
    OS << "Reserved" << getNumReservedSEW(Sew);
  else
    OS << Sew;

  unsigned LMul;
  bool IsReserved = false;
  bool Fractional = false;

  if (RISCVVType::getVLMUL(VType) == RISCVII::VLMUL::LMUL_RESERVED)
    IsReserved = true;
  else
    std::tie(LMul, Fractional) =
        RISCVVType::decodeVLMUL(RISCVVType::getVLMUL(VType));

  if (Fractional)
    OS << ", mf";
  else
    OS << ", m";

  if (IsReserved)
    OS << "Reserved";
  else
    OS << LMul;

  if (RISCVVType::isTailAgnostic(VType))
    OS << ", ta";
  else
    OS << ", tu";

  if (RISCVVType::isMaskAgnostic(VType))
    OS << ", ma";
  else
    OS << ", mu";
}

void RVVConfiguration::print(raw_ostream &OS) const {
  OS << "{ ";
  unsigned EncodedVTYPE = RISCVVType::encodeVTYPE(
      LMUL, static_cast<unsigned>(SEW), TailAgnostic, MaskAgnostic);
  printVType(EncodedVTYPE, OS);
  OS << ", vxsat: " << VxsatEnable;
  OS << ", vxrm: ";
  switch (VXRM) {
  case VXRMMode::RNU:
    OS << "rnu";
    break;
  case VXRMMode::RNE:
    OS << "rne";
    break;
  case VXRMMode::RDN:
    OS << "rdn";
    break;
  case VXRMMode::RON:
    OS << "ron";
    break;
  }

  OS << " }";
}

void RVVConfiguration::dump() const { print(dbgs()); }

struct WeightedGeneratorID {
  double Weight;
  std::string GeneratorName;
};

struct VLVMRulesDesc {
  std::vector<WeightedGeneratorID> VL;
  std::vector<WeightedGeneratorID> VM;
};

std::vector<WeightedGeneratorID>
extractWeightedGeneratorIDs(const std::vector<SList> &GenInfo) {
  std::vector<WeightedGeneratorID> Result;
  std::transform(GenInfo.begin(), GenInfo.end(), std::back_inserter(Result),
                 [](const auto &Item) {
                   if (Item.size() != 2)
                     snippy::fatal("incorrect format for genetor descriptions");
                   std::string GeneratorID = Item[0];
                   double Weight;
                   if (StringRef(Item[1]).getAsDouble(Weight))
                     snippy::fatal(Twine("could not parse weight ") +
                                   GeneratorID);
                   // TODO: more error handling
                   return WeightedGeneratorID{Weight, std::move(GeneratorID)};
                 });
  return Result;
}

static VLVMRulesDesc extractVLVMRules(const RVVUnitInfo &VMVLRules) {
  VLVMRulesDesc Result;
  Result.VL = extractWeightedGeneratorIDs(VMVLRules.VL);
  Result.VM = extractWeightedGeneratorIDs(VMVLRules.VM);
  return Result;
}

template <typename T> struct WeightedItems {
  std::vector<T> Elements;
  std::vector<double> Weights;

  void addWeightedElement(double Weight, T &&Item) {
    Elements.push_back(std::move(Item));
    Weights.push_back(Weight);
  }
};

static std::vector<InternalConfigurationPoint>
getLegalConfigurationPoints(unsigned VLEN, const RVVUnitInfo &VUInfo) {
  auto SEW = extractElementsWithPropabilities(VUInfo.VTYPE.SEW);
  auto LMUL = extractElementsWithPropabilities(VUInfo.VTYPE.LMUL);
  auto MA = extractElementsWithPropabilities(VUInfo.VTYPE.VMA);
  auto TA = extractElementsWithPropabilities(VUInfo.VTYPE.VTA);

  auto VXRM = extractElementsWithPropabilities(VUInfo.VXRM);
  auto VXSAT = extractElementsWithPropabilities(VUInfo.VXSAT);

  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "SEW", SEW.begin(), SEW.end()));
  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "LMUL", LMUL.begin(), LMUL.end()));
  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "MA", MA.begin(), MA.end()));
  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "TA", TA.begin(), TA.end()));
  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "VXRM", MA.begin(), MA.end()));
  LLVM_DEBUG(dumpRawPropabilities(dbgs(), "VXSAT", TA.begin(), TA.end()));

  std::vector<ConfigPoint> Points;
  cartesianProduct(std::back_inserter(Points), cartesianRange(SEW),
                   cartesianRange(LMUL), cartesianRange(MA), cartesianRange(TA),
                   cartesianRange(VXRM), cartesianRange(VXSAT));
  LLVM_DEBUG(dbgs() << "Raw Propabilities Points Count: " << Points.size()
                    << "\n");
  std::vector<InternalConfigurationPoint> ConfigPoints;
  std::transform(
      Points.begin(), Points.end(), std::back_inserter(ConfigPoints),
      [VLEN](const auto &Point) { return convertRepresentation(VLEN, Point); });

  // Now, at this moment ConfigPoints can contain illegal configurations
  auto RemoveIt = std::remove_if(
      ConfigPoints.begin(), ConfigPoints.end(), [](const auto &Point) {
        if (!Point.Config.IsLegal)
          LLVM_DEBUG(dbgs() << "  !!!RVV-CFG: discarding illegal config ";
                     Point.Config.print(dbgs()); dbgs() << "\n");
        return !Point.Config.IsLegal;
      });
  ConfigPoints.erase(RemoveIt, ConfigPoints.end());
  return ConfigPoints;
}

static std::vector<InternalConfigurationPoint>
getIllegalConfigurationPoints(unsigned VLEN) {
  auto AllSEW = extractElementsWithPropabilities(
      SEWInfo{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
  auto AllLMUL = extractElementsWithPropabilities(
      LMULInfo{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
  auto AllMA = extractElementsWithPropabilities(VMAInfo{1.0, 1.0});
  auto AllTA = extractElementsWithPropabilities(VTAInfo{1.0, 1.0});

  auto AllVXRM = extractElementsWithPropabilities(VXRMInfo{1.0, 1.0, 1.0, 1.0});
  auto AllVXSAT = extractElementsWithPropabilities(VXSATInfo{1.0, 1.0});

  std::vector<ConfigPoint> AllPoints;
  cartesianProduct(std::back_inserter(AllPoints), cartesianRange(AllSEW),
                   cartesianRange(AllLMUL), cartesianRange(AllMA),
                   cartesianRange(AllTA), cartesianRange(AllVXRM),
                   cartesianRange(AllVXSAT));
  std::vector<InternalConfigurationPoint> AllConfigPoints;
  std::transform(
      AllPoints.begin(), AllPoints.end(), std::back_inserter(AllConfigPoints),
      [VLEN](const auto &Point) { return convertRepresentation(VLEN, Point); });
  // Now, at this moment AllConfigPoints contain legal configurations
  auto RemoveLegalIt = std::remove_if(
      AllConfigPoints.begin(), AllConfigPoints.end(), [](const auto &Point) {
        if (NoReservedCfgRVV &&
            isReservedValues(static_cast<unsigned>(Point.Config.SEW),
                             Point.Config.LMUL))
          return true;
        return Point.Config.IsLegal;
      });
  AllConfigPoints.erase(RemoveLegalIt, AllConfigPoints.end());
  return AllConfigPoints;
}

static std::vector<InternalConfigurationPoint>
getAllConfigurationPoints(unsigned VLEN, const RVVUnitInfo &VUInfo) {
  auto ConfigPoints = getLegalConfigurationPoints(VLEN, VUInfo);
  auto IllegalConfigPoints = getIllegalConfigurationPoints(VLEN);
  // Merge two arrays with legal and illegal configurations into one common
  ConfigPoints.insert(ConfigPoints.end(), IllegalConfigPoints.begin(),
                      IllegalConfigPoints.end());
  return ConfigPoints;
}

static WeightedItems<RVVConfiguration>
getInternalConfigurationPoints(unsigned VLEN, const RVVUnitInfo &VUInfo,
                               double ProbSetVill) {
  auto ConfigPoints = getAllConfigurationPoints(VLEN, VUInfo);
  double WeightLegal = std::accumulate(
      ConfigPoints.begin(), ConfigPoints.end(), 0.0,
      [](const double Weight, const auto &Point) {
        return Weight + (Point.Config.IsLegal ? Point.Probability : 0);
      });
  double WeightIllegal = std::accumulate(
      ConfigPoints.begin(), ConfigPoints.end(), 0.0,
      [](const double Weight, const auto &Point) {
        return Weight + (Point.Config.IsLegal ? 0 : Point.Probability);
      });
  if (WeightLegal == 0 && ProbSetVill != 1)
    snippy::fatal(
        "RVV Config: no legal configuration detected and Pvill != 1, aborting");
  // If LegalWeight is zero that means that there are no legal configurations
  // and LegalWeightCoeff not needed because LegalPoints empty
  double LegalWeightCoeff = WeightLegal ? (1.0 - ProbSetVill) / WeightLegal : 0;
  double IllegalWeightCoeff = ProbSetVill / WeightIllegal;
  // We multiply all configurations by their weight coefficients.
  // This is necessary so that the sum of all probabilities is equal to 1
  std::for_each(ConfigPoints.begin(), ConfigPoints.end(),
                [LegalWeightCoeff, IllegalWeightCoeff](auto &Point) {
                  if (Point.Config.IsLegal)
                    Point.Probability *= LegalWeightCoeff;
                  else
                    Point.Probability *= IllegalWeightCoeff;
                });
  // Sort the array from highest to lowest Probabilities
  std::stable_sort(ConfigPoints.begin(), ConfigPoints.end(),
                   [](const auto &L, const auto &R) {
                     return L.Probability > R.Probability;
                   });
  // Erase Points with zero Probabilities
  auto RemoveZeroIt =
      std::remove_if(ConfigPoints.begin(), ConfigPoints.end(),
                     [](const auto &Point) { return Point.Probability == 0; });
  ConfigPoints.erase(RemoveZeroIt, ConfigPoints.end());

  LLVM_DEBUG(dbgs() << "Points Count After Cleanup: " << ConfigPoints.size()
                    << "\n");
  assert(std::abs(std::accumulate(ConfigPoints.begin(), ConfigPoints.end(), 0.0,
                                  [](const double Acc, const auto &Point) {
                                    return Acc + Point.Probability;
                                  }) -
                  1.0) < kProbabilityThreshold);

  WeightedItems<RVVConfiguration> Result;
  for (auto &&[Prob, Config] : ConfigPoints)
    Result.addWeightedElement(Prob, std::move(Config));

  return Result;
}

template <typename T>
auto constructGeneratorsFromWeightedIds(
    const std::vector<WeightedGeneratorID> &IDs) {
  WeightedItems<T> Result;
  for (const auto &GenID : IDs) {
    auto Gen = GeneratorFactory<T>::create(GenID.GeneratorName);
    if (!Gen)
      snippy::fatal("unsupported generator ID specified");
    Result.addWeightedElement(GenID.Weight, std::move(Gen));
  }
  return Result;
}

RVVConfigurationInfo
RVVConfigurationInfo::createDefault(const GeneratorSettings &GenSettings,
                                    unsigned VLEN) {
  std::vector<RVVConfiguration> Configurations = {{}};

  std::vector<VLGeneratorHolder> VLGen;
  VLGen.push_back(std::make_unique<MaxVLGenerator>());

  std::vector<VMGeneratorHolder> VMGen;
  VMGen.push_back(std::make_unique<UnmaskedVMGenerator>());

  auto ModeSwitchInfo =
      deriveModeSwitchingProbability(GenSettings.Cfg, /* mode switch bias*/ 0.0,
                                     /* set vill bit bias*/ 0.0);
  if (ModeSwitchInfo.RVVPresentInHistogram &&
      !ModeSwitchInfo.VSETPresentInHistogram) {
    snippy::fatal("No VSET instruction detected in histogram");
  }

  return RVVConfigurationInfo(
      VLEN,
      ConfigGenerator(std::move(Configurations), std::vector<double>(1, 1.0)),
      VLGenerator(std::move(VLGen), std::vector<double>(1, 1.0)),
      VMGenerator(std::move(VMGen), std::vector<double>(1, 1.0)),
      ModeSwitchInfo.SwitchInfo, !ModeSwitchInfo.VSETPresentInHistogram);
}

RVVConfigurationInfo RVVConfigurationInfo::buildConfiguration(
    const GeneratorSettings &GenSettings, unsigned VLEN,
    std::unique_ptr<RVVConfigInterface> &&Iface) {
  const RVVConfigurationSpace CS =
      Iface ? Iface->getImpl<RVVConfig>().getRVVConfigurationSpace().value()
            : GenSettings.Cfg.TargetConfig->getImpl<RISCVConfigInterface>()
                  .RVVConfig->getImpl<RVVConfig>()
                  .getRVVConfigurationSpace()
                  .value();

  if (VLEN == 0)
    snippy::fatal("RVV configuration file should not be "
                  "specified for targets without RVV");
  auto VLVMRules = extractVLVMRules(CS.VUInfo);
  auto &&[ConfigPoints, ConfigWeights] =
      getInternalConfigurationPoints(VLEN, CS.VUInfo, CS.Guides.SetVillP);
  auto &&[VLGen, VLWeights] =
      constructGeneratorsFromWeightedIds<VLGeneratorHolder>(VLVMRules.VL);
  auto &&[VMGen, VMWeights] =
      constructGeneratorsFromWeightedIds<VMGeneratorHolder>(VLVMRules.VM);

  auto ModeSwitchInfo = deriveModeSwitchingProbability(
      GenSettings.Cfg, CS.Guides.ModeChangeP, CS.Guides.SetVillP);

  if (!CS.Guides.Enabled && !ModeSwitchInfo.VSETPresentInHistogram)
    snippy::fatal("No VSET instruction detected in histogram");
  if (CS.Guides.Enabled && ModeSwitchInfo.VSETPresentInHistogram)
    snippy::fatal("It is forbidden to specify RVV mode-changing bias and "
                  "VSET* instructions in histogram simultaneously");

  return RVVConfigurationInfo(
      VLEN, ConfigGenerator(std::move(ConfigPoints), ConfigWeights),
      VLGenerator(std::move(VLGen), VLWeights),
      VMGenerator(std::move(VMGen), VMWeights), ModeSwitchInfo.SwitchInfo,
      CS.Guides.Enabled);
}

unsigned RVVConfigurationInfo::getVLEN() const { return VLEN; }

RVVConfigurationInfo::VLVM
RVVConfigurationInfo::selectVLVM(const RVVConfiguration &Config,
                                 bool ReduceVL) const {
  auto VL = VLGen()->generate(VLEN, Config);
  if (ReduceVL && (VL > kMaxVLForVSETIVLI))
    VL = kMaxVLForVSETIVLI;
  auto VM = VMGen()->generate(Config, VL);
  return {VL, VM};
}

RVVConfigurationInfo::VLVM
RVVConfigurationInfo::updateVM(const RVVConfiguration &Config,
                               const VLVM &OldVLVM) const {
  auto VL = OldVLVM.VL;
  auto VM = VMGen()->generate(Config, VL);
  return {VL, VM};
}

const RVVConfiguration &RVVConfigurationInfo::selectConfiguration() const {
  return CfgGen();
}

static std::string vsetProbInfoToString(double Prob, double InitWeight) {
  return (floatToString(Prob, 3) + Twine("[w:") + floatToString(InitWeight, 3) +
          "]")
      .str();
}

void RVVConfigurationInfo::print(raw_ostream &OS) const {
  OS << "--- RVV Configuration Info ---\n";
  OS << "  - Derived VLEN: " << VLEN << " (VLENB = " << VLEN / 8u << ")\n";
  OS << "  - Mode Change Decision Policy: ";

  if (!SwitchInfo.RVVPresent) {
    OS << "None\n";
    OS << "--- RVV Configuration End  ---\n";
    return;
  }

  if (ArtificialModeChange)
    OS << "Configuration Bias\n";
  else
    OS << "Histogram\n";

  auto Prob =
      SwitchInfo.ProbVSETVL + SwitchInfo.ProbVSETVLI + SwitchInfo.ProbVSETIVLI;
  OS << "  - Mode Change Probability: " << floatToString(Prob, 3)
     << " (vsetvl/vsetvli/vsetivli="
     << vsetProbInfoToString(SwitchInfo.ProbVSETVL, SwitchInfo.WeightVSETVL)
     << "/"
     << vsetProbInfoToString(SwitchInfo.ProbVSETVLI, SwitchInfo.WeightVSETVLI)
     << "/"
     << vsetProbInfoToString(SwitchInfo.ProbVSETIVLI, SwitchInfo.WeightVSETIVLI)
     << ")"
     << "\n";
  OS << "    ";
  OS << "Set Vill Bit Probability: " << floatToString(SwitchInfo.ProbSetVill, 3)
     << "\n";

  OS << "  - VL Selection Rules:\n";
  for (const auto &[Gen, Prob] :
       llvm::zip(VLGen.elements(), VLGen.probabilities())) {
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5) << " ";
    OS << "<" << Gen->identify() << ">\n";
  }
  OS << "  - VM Selection Rules:\n";
  for (const auto &[Gen, Prob] :
       llvm::zip(VMGen.elements(), VMGen.probabilities())) {
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5) << " ";
    OS << "<" << Gen->identify() << ">\n";
  }

  OS << "  - Configuration Bag Listing:\n";
  unsigned IllegalPointsSize = 0;
  for (const auto &[Point, Prob] :
       llvm::zip(CfgGen.elements(), CfgGen.probabilities())) {
    if (!Point.IsLegal) {
      ++IllegalPointsSize;
      continue;
    }
    OS << "    ";
    OS << "P: " << floatToString(Prob, 5);
    OS << " Conf: ";
    Point.print(OS);
    OS << "/MaxVL: "
       << computeVLMax(VLEN, static_cast<unsigned>(Point.SEW), Point.LMUL);
    OS << "\n";
  }
  if (IllegalPointsSize > 0) {
    OS << "    ";
    OS << "P: " << floatToString(SwitchInfo.ProbSetVill, 5) << " Conf: ";
    OS << "{  Illegal Configurations:  " << IllegalPointsSize
       << " points }/MaxVL: 0\n";
  }
  OS << "  - Configuration Bag Size: " << CfgGen.elements().size() << "\n";
  // Now, lets try to get an approximate cardianality of the set
  auto Cardinality =
      std::accumulate(CfgGen.elements().begin(), CfgGen.elements().end(),
                      size_t(0), [this](const auto &Acc, const auto &Point) {
                        auto PointSEW = static_cast<unsigned>(Point.SEW);
                        auto MaxVL = computeVLMax(VLEN, PointSEW, Point.LMUL);
                        return Acc + MaxVL;
                      });
  OS << "  - State Cardinality: " << Cardinality << " ~ {MASKS} \n";
  OS << "--- RVV Configuration End  ---\n";
}

void RVVConfigurationInfo::dump() const { print(dbgs()); }

RVVConfigurationInfo::RVVConfigurationInfo(
    unsigned VLEN, ConfigGenerator &&CfgGen, VLGenerator &&VLGen,
    VMGenerator &&VMGen, const ModeChangeInfo &SwitchInfo, bool EnableGuides)
    : VLEN(VLEN), CfgGen(std::move(CfgGen)), VLGen(std::move(VLGen)),
      VMGen(std::move(VMGen)), SwitchInfo(SwitchInfo),
      ArtificialModeChange(EnableGuides) {}

RISCVConfigurationInfo RISCVConfigurationInfo::constructConfiguration(
    LLVMState &State, const GeneratorSettings &GenSettings) {
  auto &Ctx = State.getCtx();
  const auto &TM = State.getTargetMachine();
  auto ArchInfo =
      RISCVConfigurationInfo::deriveArchitecturalInformation(Ctx, TM);
  auto BaseCfg = BaseConfigurationInfo(ArchInfo.XLEN);
  auto RVVCfg =
      GenSettings.Cfg.TargetConfig->getImpl<RISCVConfigInterface>()
              .RVVConfig->getImpl<RVVConfig>()
              .getRVVConfigurationSpace()
              .has_value()
          ? RVVConfigurationInfo::buildConfiguration(GenSettings, ArchInfo.VLEN,
                                                     nullptr)
          : RVVConfigurationInfo::createDefault(GenSettings, ArchInfo.VLEN);
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

  if (!UseNonSimplifiedRVVConfig) {
    Result.VLEN = SimplifiedRVV_VLEN;
    return Result;
  }

  Result.VLEN = ST.getRealMaxVLen();
  return Result;
}

} // namespace snippy
} // namespace llvm
