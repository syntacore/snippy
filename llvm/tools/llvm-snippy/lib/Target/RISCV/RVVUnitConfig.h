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
class GeneratorSettings;

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

enum RVVConstants { kMaxVLForVSETIVLI = 31u };

struct RVVConfiguration final {
  bool IsLegal = true;
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
  unsigned static getMaxLMUL() {
    return static_cast<unsigned>(RISCVII::VLMUL::LMUL_8);
  }

  RISCVII::VLMUL LMUL = RISCVII::VLMUL::LMUL_1;
  VSEW SEW = VSEW::SEW64;
  bool MaskAgnostic = false;
  bool TailAgnostic = false;

  VXRMMode VXRM = VXRMMode::RNU;
  bool VxsatEnable = false;

  void print(raw_ostream &OS) const;
  void dump() const;
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
  virtual unsigned generate(unsigned VLEN,
                            const RVVConfiguration &Cfg) const = 0;
  virtual ~VLGeneratorInterface(){};
};

struct VMGeneratorInterface {
  virtual std::string identify() const = 0;
  virtual APInt generate(const RVVConfiguration &Cfg, unsigned VL) const = 0;
  virtual ~VMGeneratorInterface(){};
};

class GeneratorContext;

struct ModeChangeInfo {
  bool RVVPresent = false;

  // Probability with which an illegal RVVConfiguration will be choosen during
  // RVV Mode Change
  double ProbSetVill = 0.0;

  // ProbVSET* are not expected to be used by external clients
  // These are only for printing and debugging purposes
  double ProbVSETVL = 0.0;
  double ProbVSETVLI = 0.0;
  double ProbVSETIVLI = 0.0;

  // Weights are what our clients are expected to use. llvm-snippy uses
  // weight-based histograms for instruction selection. These weights are later
  // used to calculate probabilities for the discrete distribution.
  // Depending on the context, weights can represent slightly different
  // things:
  //    1. If we use histogram-based RVV mode selection, these weights are used
  // to construct the probability of the first VSET* instruction for the basic
  // block. The respected weight is proportional to the number of RVV
  // instructions in the histogram.
  //    2. In case of a bias-based mode selection, these weights are used
  // for a direct initialization of the distribution. The final weight is
  // calculated as (ALL_INSTR_WEIGHTS * BIAS_COEFF).
  // For example: if BIAS_COEFF (P) = 1.0, then the expected probability to
  // generate VSET* is about 50% regardless of the number of instructions in
  // the histogram.
  double WeightVSETVL = 0.0;
  double WeightVSETVLI = 0.0;
  double WeightVSETIVLI = 0.0;
};

struct RVVConfigurationInfo final {

  struct VLVM {
    unsigned VL;
    APInt VM;
  };

  static RVVConfigurationInfo
  createDefault(const GeneratorSettings &GenSettings, unsigned VLEN);

  static RVVConfigurationInfo
  buildConfiguration(const GeneratorSettings &GenSettings, unsigned VLEN,
                     std::unique_ptr<RVVConfigInterface> &&VU);

  unsigned getVLEN() const;
  unsigned getVLENB() const { return getVLEN() / 8; }

  // Randomly choose VL and VM from rvv config.
  VLVM selectVLVM(const RVVConfiguration &Config, bool ReducedVL) const;

  // Choose only VM from available values, VL stays the same.
  VLVM updateVM(const RVVConfiguration &Config, const VLVM &OldVLVM) const;

  const RVVConfiguration &selectConfiguration() const;

  bool isModeChangeArtificial() const { return ArtificialModeChange; }
  const ModeChangeInfo &getModeChangeInfo() const { return SwitchInfo; }

  const std::vector<RVVConfiguration> &getConfigs() const {
    return CfgGen.elements();
  }

  void print(raw_ostream &OS) const;
  void dump() const;

  using VLGeneratorHolder = std::unique_ptr<VLGeneratorInterface>;
  using VMGeneratorHolder = std::unique_ptr<VMGeneratorInterface>;

private:
  using ConfigGenerator = DiscreteGeneratorInfo<RVVConfiguration>;
  using VLGenerator = DiscreteGeneratorInfo<VLGeneratorHolder>;
  using VMGenerator = DiscreteGeneratorInfo<VMGeneratorHolder>;

  RVVConfigurationInfo(unsigned VLEN, ConfigGenerator &&CfgGen,
                       VLGenerator &&VLGen, VMGenerator &&VMGen,
                       const ModeChangeInfo &SwitchInfo, bool EnableGuides);

  unsigned VLEN;
  ConfigGenerator CfgGen;
  VLGenerator VLGen;
  VMGenerator VMGen;
  ModeChangeInfo SwitchInfo;
  bool ArtificialModeChange;
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
  static RISCVConfigurationInfo
  constructConfiguration(LLVMState &State,
                         const GeneratorSettings &GenSettings);
  const RVVConfigurationInfo &getVUConfig() const { return RVVCfgInfo; }
  const BaseConfigurationInfo &getBaseConfig() const { return BaseCfgInfo; }
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_RVV_UNIT_CONFIG_H
