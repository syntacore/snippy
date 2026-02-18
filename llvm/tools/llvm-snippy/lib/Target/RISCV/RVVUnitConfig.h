#ifndef LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_RVV_UNIT_CONFIG_H
#define LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_RVV_UNIT_CONFIG_H

//===-- RVVUnitConfig.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "snippy/Simulator/Targets/RISCV.h"
#include "snippy/Support/RandUtil.h"

#include <iterator>
#include <string>
#include <vector>

namespace llvm {
class StringRef;
class raw_ostream;
class RISCVSubtarget;
class TargetMachine;
class LLVMContext;

namespace yaml {
class IO;
}

} // namespace llvm

namespace llvm {

struct RVVConfigInterface {
  virtual ~RVVConfigInterface(){};

  template <typename ImplT> ImplT &getImpl() {
    return static_cast<ImplT &>(*this);
  }

  template <typename ImplT> const ImplT &getImpl() const {
    return static_cast<const ImplT &>(*this);
  }

  virtual bool hasConfig() const = 0;
  virtual void mapYaml(yaml::IO &IO) = 0;
};

namespace snippy {

class LLVMState;
class Config;

std::unique_ptr<RVVConfigInterface> createRVVConfig();

// Compute EMUL = EEW / SEW * LMUL
RISCVII::VLMUL computeEMUL(unsigned SEW, unsigned EEW, RISCVII::VLMUL LMUL);
std::pair<unsigned, bool> computeDecodedEMUL(unsigned SEW, unsigned EEW,
                                             RISCVII::VLMUL LMUL);
bool isValidEMUL(unsigned SEW, unsigned EEW, RISCVII::VLMUL LMUL);

inline static bool canBeEncoded(unsigned SEW) {
  // This wrapper clarify the meaning of the function RISCVVType::isValidSEW.
  // It returns true when we can encoded the SEW (reserved and not)
  return RISCVVType::isValidSEW(SEW);
}

// NOTE:
// VLEN - target VLEN
// ELEN - maximum supported element width
// LMUL - lmul for which we want to compute VLMAX
// If function returns zero - it means that such combination is not valid
// TODO: consider replacing unsigned `SEW` parameter with typed enum
unsigned computeVLMax(unsigned VLEN, unsigned SEW, RISCVII::VLMUL LMUL);

enum RVVConstants { kMaxVLForVSETIVLI = 31u, kMaxLMUL = 8u };

struct RVVConfiguration final {
  // Note: integer values are in-sync with RVV spec 1.0
  enum class VXRMMode : unsigned { RNU = 0, RNE = 1, RDN = 2, RON = 3 };
  enum class VSEW : unsigned {
    SEW8 = 8,
    SEW16 = 16,
    SEW32 = 32,
    SEW64 = 64,
    SEWReserved1 = 128,
    SEWReserved2 = 256,
    SEWReserved3 = 512,
    SEWReserved4 = 1024
  };

  unsigned static getMinSEW() { return static_cast<unsigned>(VSEW::SEW8); }
  unsigned static getMaxLMUL() { return RVVConstants::kMaxLMUL; }

  bool IsLegal = true;
  RISCVII::VLMUL LMUL = RISCVII::VLMUL::LMUL_1;
  VSEW SEW = VSEW::SEW64;
  bool MaskAgnostic = false;
  bool TailAgnostic = false;

  VXRMMode VXRM = VXRMMode::RNU;

  void print(raw_ostream &OS) const;
  void dump() const;

  // TODO: Typically RVVConfigurations are equal if and only if their addresses
  // are equal (i.e. they are the same object). However, there is a rare case
  // when we are comparing SupportConfig and config from the main pool, they
  // have different addresses, but might have the same content.
  // Is it worth it to optimize this and keep this operator?
  bool operator==(const RVVConfiguration &Other) const {
    return IsLegal == Other.IsLegal && LMUL == Other.LMUL && SEW == Other.SEW &&
           MaskAgnostic == Other.MaskAgnostic &&
           TailAgnostic == Other.TailAgnostic && VXRM == Other.VXRM;
  }

  bool operator!=(const RVVConfiguration &Other) const {
    return !(*this == Other);
  }
};

inline static bool isLegalSEW(unsigned SEW) {
  auto SEWEnum = static_cast<RVVConfiguration::VSEW>(SEW);
  switch (SEWEnum) {
  default:
    return false;
  case RVVConfiguration::VSEW::SEW8:
  case RVVConfiguration::VSEW::SEW16:
  case RVVConfiguration::VSEW::SEW32:
  case RVVConfiguration::VSEW::SEW64:
    return true;
  }
}

// TODO: this should belong to RandUtils
template <typename ElementType,
          typename ContainerType = std::vector<ElementType>>
struct DiscreteGeneratorInfo final {
  template <typename InputIt>
  DiscreteGeneratorInfo(ContainerType &&ElementsIn, InputIt First, InputIt Last)
      : Elements(std::move(ElementsIn)), Distribution(First, Last) {
    using ItValueType = typename std::iterator_traits<InputIt>::value_type;
    static_assert(std::is_same_v<ItValueType, double>,
                  "Weights are expected to be of double type");
    assert(static_cast<size_t>(std::distance(First, Last)) == Elements.size());
    assert(!Elements.empty());
  }

  template <typename WeightsType>
  DiscreteGeneratorInfo(ContainerType &&ElementsIn, const WeightsType &Weights)
      : DiscreteGeneratorInfo(std::move(ElementsIn), Weights.begin(),
                              Weights.end()) {}

  const ElementType &operator()() const {
    auto ItemIdx = Distribution(RandEngine::engine());
    assert(ItemIdx < Elements.size());
    return Elements[ItemIdx];
  }

  // Generates an element that satisfies an unary predicate P.
  // If there is no such element, returns nullopt.
  template <typename UnaryPredicate>
  std::optional<const std::reference_wrapper<const ElementType>>
  generateIf(UnaryPredicate P) const {
    std::vector<unsigned> FilteredIndexes;
    std::vector<double> FilteredProbs;
    const auto &Probs = probabilities();
    for (auto [Idx, Elem] : enumerate(Elements)) {
      if (!P(Elem))
        continue;
      FilteredIndexes.emplace_back(Idx);
      FilteredProbs.emplace_back(Probs[Idx]);
    }
    if (FilteredIndexes.empty())
      return std::nullopt;
    auto SelectedElemIdx = DiscreteGeneratorInfo<unsigned>(
        std::move(FilteredIndexes), std::move(FilteredProbs))();
    return std::cref<const ElementType>(Elements[SelectedElemIdx]);
  }

  const ContainerType &elements() const { return Elements; }

  std::vector<double> probabilities() const {
    return Distribution.probabilities();
  }

private:
  ContainerType Elements;
  // std::discrete_distribution::operator() is not const!!!
  mutable std::discrete_distribution<size_t> Distribution;
};

struct VLGeneratorInterface {
  virtual std::string identify() const = 0;
  virtual bool isApplicable(unsigned /* VLEN */, bool /* ReduceVL */,
                            const RVVConfiguration & /* Cfg */) const {
    return true;
  };
  virtual unsigned generate(unsigned VLEN,
                            const RVVConfiguration &Cfg) const = 0;
  virtual ~VLGeneratorInterface(){};
};

struct VMGeneratorInterface {
  virtual std::string identify() const = 0;
  virtual bool isApplicable(unsigned /* VL */) const { return true; }
  virtual APInt generate(unsigned VL) const = 0;
  virtual ~VMGeneratorInterface(){};
};

class GeneratorContext;

struct ModeChangeInfo {
  bool RVVPresentInHistogram = false;
  bool VSETPresentInHistogram = false;

  // Probability with which an illegal RVVConfiguration will be choosen during
  // RVV Mode Change
  double ProbSetVill = 0.0;

  // Weights are what our clients are expected to use. llvm-snippy uses
  // weight-based histograms for instruction selection. These weights are later
  // used to calculate probabilities for the discrete distribution.
  //
  // The values either come directly from the histogram, or calculated from
  // BIAS_COEFF, which shows the ratio between support VSET* instructions
  // and all other instructions. For example, if BIAS_COEFF is 0.8 and
  // num-instrs=100, there will be 80 support VSET* instructions in addition to
  // 100 requested instructions.
  //
  // Keep in mind that if weights come from the histogram (e.g. VSETs are
  // primary instructions), they are contributing to the total weight
  // of the histogram, so we need to account for that when calculating
  // the amount of VSET* instructions (mode-changing groups).
  double WeightVSETVL = 0.0;
  double WeightVSETVLI = 0.0;
  double WeightVSETIVLI = 0.0;

  double TotalHistWeight = 0.0;

  // Here 'probability' means ratio of VSET* instructions to the union of all
  // primary instructions and VSET* instructions, so this 'probability' depends
  // on whether VSET* instructions are primary or not.
  double getWeightToProbabilityMultiplier() const {
    assert(TotalHistWeight > 0.0);
    if (VSETPresentInHistogram)
      return 1.0 / TotalHistWeight;
    auto TotalVSETWeight = WeightVSETVL + WeightVSETVLI + WeightVSETIVLI;
    return 1.0 / (TotalHistWeight + TotalVSETWeight);
  }

  // Even if all weights are 0.0, we still might need to choose one of VSETs
  std::array<double, 3> getRelativeWeights(unsigned VL) const {
    if (WeightVSETVL + WeightVSETVLI + WeightVSETIVLI <=
        std::numeric_limits<double>::epsilon())
      return {1.0, 1.0, 1.0};

    // VSETIVLI supports only reduced VL
    if (VL > kMaxVLForVSETIVLI)
      return {WeightVSETVL, WeightVSETVLI, 0.0};
    return {WeightVSETVL, WeightVSETVLI, WeightVSETIVLI};
  }
};

struct RVVConfigurationInfo final {

  struct VLVM {
    unsigned VL;
    APInt VM;

    bool operator==(const VLVM &Other) const {
      return VL == Other.VL && APInt::isSameValue(VM, Other.VM);
    }
  };

  using VLGeneratorHolder = std::unique_ptr<VLGeneratorInterface>;
  using VMGeneratorHolder = std::unique_ptr<VMGeneratorInterface>;

  static RVVConfigurationInfo createDefault(const Config &Cfg, unsigned VLEN);

  static RVVConfigurationInfo
  buildConfiguration(const Config &Cfg, unsigned VLEN,
                     std::unique_ptr<RVVConfigInterface> &&VU,
                     std::vector<VMGeneratorHolder> &DiscardedVMs,
                     std::vector<VLGeneratorHolder> &DiscardedVLs,
                     std::vector<RVVConfiguration> &DiscardedConfigs);

  unsigned getVLEN() const;
  unsigned getVLENB() const { return getVLEN() / RISCV_CHAR_BIT; }

  // Randomly choose VL and VM from rvv config.
  VLVM selectVLVM(const RVVConfiguration &Config, bool ReducedVL) const;

  // Choose only VM from available values, VL stays the same.
  VLVM updateVM(const RVVConfiguration &Config, const VLVM &OldVLVM) const;

  const RVVConfiguration &selectConfiguration() const;

  bool isModeChangeArtificial() const { return ArtificialModeChange; }
  bool isVXRMUpdateNeeded() const { return NeedsVXRMUpdate; }
  const ModeChangeInfo &getModeChangeInfo() const { return SwitchInfo; }

  const std::vector<RVVConfiguration> &getConfigs() const {
    return CfgGen.elements();
  }

  void print(raw_ostream &OS) const;
  void dump() const;

private:
  using ConfigGenerator = DiscreteGeneratorInfo<RVVConfiguration>;
  using VLGenerator = DiscreteGeneratorInfo<VLGeneratorHolder>;
  using VMGenerator = DiscreteGeneratorInfo<VMGeneratorHolder>;

  RVVConfigurationInfo(unsigned VLEN, ConfigGenerator &&CfgGen,
                       VLGenerator &&VLGen, VMGenerator &&VMGen,
                       const ModeChangeInfo &SwitchInfo, bool EnableGuides,
                       bool NeedsVXRMUpdate);

  const VLGeneratorHolder &selectVLGen(const RVVConfiguration &Config,
                                       bool ReduceVL) const;
  const VMGeneratorHolder &selectVMGen(unsigned VL) const;

  unsigned VLEN;
  ConfigGenerator CfgGen;
  VLGenerator VLGen;
  VMGenerator VMGen;
  ModeChangeInfo SwitchInfo;
  bool ArtificialModeChange;
  bool NeedsVXRMUpdate;

public:
  // Support configuration can be temporarily used for support sequences, like
  // writing to a register. After that the previous configuration is restored.
  constexpr static RVVConfiguration SupportCfgSew64{
      true /*IsLegal*/, RISCVII::VLMUL::LMUL_1, RVVConfiguration::VSEW::SEW64};
  constexpr static RVVConfiguration SupportCfgSew32{
      true /*IsLegal*/, RISCVII::VLMUL::LMUL_1, RVVConfiguration::VSEW::SEW32};
};

class BaseConfigurationInfo final {
  unsigned XLEN;

public:
  BaseConfigurationInfo(unsigned XLenIn) : XLEN(XLenIn) {}

  unsigned getXLEN() const { return XLEN; };
};

class RISCVConfigurationInfo final {
  struct ArchitecturalInfo {
    unsigned VLEN = 0;
    unsigned XLEN = 0;
  };

  BaseConfigurationInfo BaseCfgInfo;
  RVVConfigurationInfo RVVCfgInfo;

  RISCVConfigurationInfo(BaseConfigurationInfo &&BaseCfgInfoIn,
                         RVVConfigurationInfo &&RVVCfgInfoIn)
      : BaseCfgInfo(std::move(BaseCfgInfoIn)),
        RVVCfgInfo(std::move(RVVCfgInfoIn)) {}
  static ArchitecturalInfo
  deriveArchitecturalInformation(LLVMContext &Ctx, const TargetMachine &TM);

public:
  static RISCVConfigurationInfo constructConfiguration(LLVMState &State,
                                                       const Config &Cfg);
  const RVVConfigurationInfo &getVUConfig() const { return RVVCfgInfo; }
  const BaseConfigurationInfo &getBaseConfig() const { return BaseCfgInfo; }
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_RVV_UNIT_CONFIG_H
