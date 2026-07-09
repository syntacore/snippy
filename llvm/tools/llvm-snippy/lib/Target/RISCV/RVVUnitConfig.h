//===-- RVVUnitConfig.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RVVUNITCONFIG_H
#define LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RVVUNITCONFIG_H

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "snippy/Simulator/Targets/RISCV.h"
#include "snippy/Support/ProbabilityUtils.h"
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
} // namespace yaml

struct RVVConfigInterface {
  virtual ~RVVConfigInterface() {};

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

enum RVVConstants { kMaxVLForVSETIVLI = 31u, kMaxLMUL = 8u };

// Enum lists are necessary for mapping and validation during YAML parsing

using VLMUL = llvm::RISCVVType::VLMUL;
struct LMULEnumList final {
  static constexpr std::array Arr = {
      VLMUL::LMUL_1,        VLMUL::LMUL_2,  VLMUL::LMUL_4,  VLMUL::LMUL_8,
      VLMUL::LMUL_RESERVED, VLMUL::LMUL_F8, VLMUL::LMUL_F4, VLMUL::LMUL_F2};
};

// Note: integer values are in-sync with RVV spec 1.0
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
struct SEWEnumList final {
  static constexpr std::array Arr = {VSEW::SEW8,         VSEW::SEW16,
                                     VSEW::SEW32,        VSEW::SEW64,
                                     VSEW::SEWReserved1, VSEW::SEWReserved2,
                                     VSEW::SEWReserved3, VSEW::SEWReserved4};
};

enum class VXRMMode : unsigned { RNU = 0, RNE = 1, RDN = 2, RON = 3 };
struct VXRMEnumList final {
  static constexpr std::array Arr = {VXRMMode::RNU, VXRMMode::RNE,
                                     VXRMMode::RDN, VXRMMode::RON};
};

enum class VMAMode : unsigned { MU = 0, MA = 1 };
struct VMAEnumList final {
  static constexpr std::array Arr = {VMAMode::MA, VMAMode::MU};
};

enum class VTAMode : unsigned { TU = 0, TA = 1 };
struct VTAEnumList final {
  static constexpr std::array Arr = {VTAMode::TA, VTAMode::TU};
};

class LLVMState;
class Config;

std::unique_ptr<RVVConfigInterface> createRVVConfig();

// Compute EMUL = EEW / SEW * LMUL
VLMUL computeEMUL(unsigned ELEN, unsigned SEW, unsigned EEW, VLMUL LMUL);
std::pair<unsigned, bool> computeDecodedEMUL(unsigned ELEN, unsigned SEW,
                                             unsigned EEW, VLMUL LMUL);
bool isValidEMUL(unsigned ELEN, unsigned SEW, unsigned EEW, VLMUL LMUL);

inline static bool canBeEncoded(unsigned SEW) {
  // This wrapper clarify the meaning of the function RISCVVType::isValidSEW.
  // It returns true when we can encoded the SEW (reserved and not)
  return RISCVVType::isValidSEW(SEW);
}

inline bool isLegalSEW(VSEW SEW) {
  switch (SEW) {
  default:
    return false;
  case VSEW::SEW8:
  case VSEW::SEW16:
  case VSEW::SEW32:
  case VSEW::SEW64:
    return true;
  }
}

inline bool isLegalSEW(unsigned SEW) {
  return isLegalSEW(static_cast<VSEW>(SEW));
}

inline bool isLegalLMUL(VLMUL LMUL) { return LMUL != VLMUL::LMUL_RESERVED; }

// Illegal configuration occurs when the {SEW, LMUL} pair violates the target's
// constraints based on VLEN and ELEN. It happens when:
// - LMUL is a reserved value (not 1/8, 1/4, 1/2, 1, 2, 4, 8)
// - SEW is a reserved value (not 8, 16, 32, 64) or SEW > ELEN
// - LMUL * VLEN < SEW (not enough space for one element)
// - LMUL < SEW / ELEN
// Such configs set the vill bit in vtype.
bool isLegalSewLmul(unsigned ELEN, unsigned VLEN, VSEW SEW, VLMUL LMUL);

unsigned computeVLMax(unsigned ELEN, unsigned VLEN, VSEW SEW, VLMUL LMUL);

constexpr VSEW getMinSEW() { return VSEW::SEW8; }
constexpr VLMUL getMaxLMUL() { return VLMUL::LMUL_8; }

inline unsigned getMaxPossibleVL(unsigned ELEN, unsigned VLEN) {
  return computeVLMax(ELEN, VLEN, getMinSEW(), getMaxLMUL());
}

// Values that define legality of RVV instructions with this mode.
struct RVVPrimaryConfig {
  VSEW SEW = VSEW::SEW64;
  VLMUL LMUL = VLMUL::LMUL_1;
  unsigned VL = 0;

  bool isLegal(unsigned ELEN, unsigned VLEN) const {
    auto VLMax = computeVLMax(ELEN, VLEN, SEW, LMUL);
    if (VLMax == 0)
      return false;
    return VL <= VLMax;
  }

  bool operator==(const RVVPrimaryConfig &Other) const {
    return SEW == Other.SEW && LMUL == Other.LMUL && VL == Other.VL;
  }
};

struct RVVConfiguration final {
  RVVPrimaryConfig PrimaryCfg;
  // Other things that do not affect legality
  VMAMode MA = VMAMode::MU;
  VTAMode TA = VTAMode::TU;
  VXRMMode XRM = VXRMMode::RNU;

  std::string toStr() const;
  void print(raw_ostream &OS) const { OS << toStr() << "\n"; };

  bool isLegal(unsigned ELEN, unsigned VLEN) const {
    return PrimaryCfg.isLegal(ELEN, VLEN);
  }

  bool operator==(const RVVConfiguration &Other) const {
    return PrimaryCfg == Other.PrimaryCfg && MA == Other.MA && TA == Other.TA &&
           XRM == Other.XRM;
  }

  unsigned getVTYPE() const {
    bool IsTA = (TA == VTAMode::TA);
    bool IsMA = (MA == VMAMode::MA);
    return RISCVVType::encodeVTYPE(
        PrimaryCfg.LMUL, static_cast<unsigned>(PrimaryCfg.SEW), IsTA, IsMA);
  }

  static RVVConfiguration fromVTYPE(unsigned VL, unsigned VTYPE, VXRMMode XRM) {
    VLMUL LMUL = RISCVVType::getVLMUL(VTYPE);
    VSEW SEW = static_cast<VSEW>(RISCVVType::getSEW(VTYPE));
    bool IsMA = RISCVVType::isMaskAgnostic(VTYPE);
    bool IsTA = RISCVVType::isTailAgnostic(VTYPE);

    return RVVConfiguration{RVVPrimaryConfig{SEW, LMUL, VL},
                            IsMA ? VMAMode::MA : VMAMode::MU,
                            IsTA ? VTAMode::TA : VTAMode::TU, XRM};
  }
};

// GlobalMaxVL depends on VLEN, we can't know it at compile time.
using VLDistributionType = std::vector<double>;

struct VLGeneratorInterface {
  virtual std::string identify() const = 0;
  // Returns vector of probabilities for VL values from 0 to VLMax (inclusive).
  // The size of the vector must be VLMax + 1.
  // The sum of these probabilities must be either 1.0 or 0.0 (meaning that this
  // generator is not applicable for this VLMax).
  virtual VLDistributionType getDistribution(unsigned VLMax) const = 0;

  // We have opcode-dependent VL generator "vlmax". It's the only
  // one whose distribution is different for VSETVL & VSETVLI vs VSETIVLI. For
  // this reason this method must be overridden for "vlmax" generator.
  //
  // For now simply using getDistribution and squashing all Vls >
  // kMaxVLForVSETIVLI to kMaxVLForVSETIVLI. This represents the old behavior.
  virtual VLDistributionType getDistributionForVSETIVLI(unsigned VLMax) const {
    auto FullDist = getDistribution(VLMax);
    VLDistributionType Result(kMaxVLForVSETIVLI + 1);

    // Copy as much of FullDist as possible into Result
    auto MinSize = std::min(FullDist.size(), Result.size());
    std::copy_n(FullDist.begin(), MinSize, Result.begin());

    // Add all weight past kMaxVLForVSETIVLI to the last element
    if (FullDist.size() > Result.size())
      Result.back() += std::accumulate(FullDist.begin() + Result.size(),
                                       FullDist.end(), 0.0);

    return Result;
  }

  virtual ~VLGeneratorInterface() {};
};
using VLGeneratorHolder = std::unique_ptr<VLGeneratorInterface>;

struct VMGeneratorInterface {
  virtual std::string identify() const = 0;
  // We can apply a mask only if the number of active bits in it
  // does not exceed the total number of elements (VL).
  //
  // For example,
  //             active 8 bits/total 11 bits
  //                <------>
  //   ImmVM = 0b00010011110 (158)
  //   APInt(/* numBits */ 11, /* val */ 158).getActiveBits() == 8
  //   If number of elements (VL) >= 8 this mask is applicable, otherwise not.
  virtual unsigned getMinRequiredVL() const = 0;
  virtual APInt generate(unsigned VL) const = 0;
  virtual ~VMGeneratorInterface() {};
};
using VMGeneratorHolder = std::unique_ptr<VMGeneratorInterface>;

class GeneratorContext;

struct ModeChangeInfo {
  bool RVVPresentInHistogram = false;
  bool VSETPresentInHistogram = false;

  // Probability with which an illegal RVV configuration will be choosen during
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

  // Returns weights in the form of {VSETVL, VSETVLI, VSETIVLI}
  // Even if all weights are 0.0, we still might need to choose one of VSETs
  std::array<double, 3> getRelativeWeights(unsigned VL = 0) const {
    if (isZero(WeightVSETVL + WeightVSETVLI + WeightVSETIVLI))
      return {1.0, 1.0, 1.0};

    // VSETIVLI supports only reduced VL
    if (VL > kMaxVLForVSETIVLI)
      return {WeightVSETVL, WeightVSETVLI, 0.0};
    return {WeightVSETVL, WeightVSETVLI, WeightVSETIVLI};
  }

  // Returns probabilities in the form of {VSETVL, VSETVLI, VSETIVLI}
  // The sum of all probabilities is 1.0
  std::array<double, 3> getRelativeProbabilities() const {
    auto Weights = getRelativeWeights();
    normalizeValues(Weights);
    return Weights;
  }

  // Some functions for readability
  bool hasVSETVL() const {
    auto [WeightVSETVL, WeightVSETVLI, WeightVSETIVLI] = getRelativeWeights();
    return WeightVSETVL > 0.0;
  }
  bool hasVSETVLI() const {
    auto [WeightVSETVL, WeightVSETVLI, WeightVSETIVLI] = getRelativeWeights();
    return WeightVSETVLI > 0.0;
  }
  bool hasVSETIVLI() const {
    auto [WeightVSETVL, WeightVSETVLI, WeightVSETIVLI] = getRelativeWeights();
    return WeightVSETIVLI > 0.0;
  }
};

using SEWInfo = WeightsArray<SEWEnumList>;
using LMULInfo = WeightsArray<LMULEnumList>;
using VMAInfo = WeightsArray<VMAEnumList>;
using VTAInfo = WeightsArray<VTAEnumList>;
using VXRMInfo = WeightsArray<VXRMEnumList>;

struct VTypeInfo {
  SEWInfo SEW;
  LMULInfo LMUL;
  VMAInfo VMA;
  VTAInfo VTA;
};

struct VLVMSequence final {
  using VLVMEntry = std::pair<std::string, double>;
  std::vector<VLVMEntry> Values;
};

struct RVVUnitInfo {
  VXRMInfo VXRM;
  VTypeInfo VTYPE;

  VLVMSequence VM;
  VLVMSequence VL;
};

// Simply providing connection between Idx in storage and {SEW, LMUL, VL}
// combination. Using row-major order: SEW iterates slowest, VL fastest
struct PrimaryConfigMapping final {
  static constexpr size_t LMULSize = LMULEnumList::Arr.size();
  static constexpr size_t SEWSize = SEWEnumList::Arr.size();
  const size_t VLSize;

  size_t toIdx(VSEW SEW, VLMUL LMUL, unsigned VL) const {
    assert(VL < VLSize);
    unsigned SEWIdx = SEWInfo::Mapping::toIdx(SEW);
    unsigned LMULIdx = LMULInfo::Mapping::toIdx(LMUL);
    return VLSize * (LMULSize * SEWIdx + LMULIdx) + VL;
  }

  RVVPrimaryConfig idxToConfig(size_t Idx) const {
    assert(Idx < VLSize * LMULSize * SEWSize);
    size_t SEWIdx = Idx / (VLSize * LMULSize);
    size_t Rem = Idx % (VLSize * LMULSize);
    size_t LMULIdx = Rem / VLSize;
    unsigned VL = Rem % VLSize;
    return {SEWEnumList::Arr[SEWIdx], LMULEnumList::Arr[LMULIdx], VL};
  }

  PrimaryConfigMapping(size_t VLSize) : VLSize(VLSize) {}
};

using SewLmulDistribution = ProbableItems<std::tuple<VSEW, VLMUL>>;

struct PrimaryWeightsAndMapping {
  std::vector<double> Weights;
  PrimaryConfigMapping Mapping;

  // Returns all configs with non-zero probabilities
  [[nodiscard]] ProbableItems<RVVPrimaryConfig> getAllPossibleConfigs() const;
  // Returns all {SEW, LMUL} pairs with non-zero probabilities
  [[nodiscard]] SewLmulDistribution getAllPossibleSewLmulPairs() const;

  void printProbabilities(raw_ostream &OS) const;

  // Addition of two distributions. Expects the other distribution to be smaller
  PrimaryWeightsAndMapping &operator+=(const PrimaryWeightsAndMapping &Other) {
    assert(Weights.size() >= Other.Weights.size());

    // Add weights of the smaller distribution to the larger one.
    for (const auto &[Idx, Weight] : enumerate(Other.Weights)) {
      auto [SEW, LMUL, VL] = Other.Mapping.idxToConfig(Idx);
      size_t ThisIdx = Mapping.toIdx(SEW, LMUL, VL);
      Weights[ThisIdx] += Weight;
    }
    return *this;
  }

  // Multiply all weights by a factor
  PrimaryWeightsAndMapping &operator*=(double Factor) {
    for (auto &Weight : Weights)
      Weight *= Factor;
    return *this;
  }
};

struct RVVPrimaryConfigGenerator final {
  static constexpr size_t LMULSize = LMULEnumList::Arr.size();
  static constexpr size_t SEWSize = SEWEnumList::Arr.size();
  // VL can have values [0, GlobalVLMax]
  const size_t VLSize;
  // When creating distribution for VSETIVLI:
  //   VLSize = kMaxVLForVSETIVLI + 1
  // When creating distribution for VSETVL & VSETVLI:
  //   VLSize = GlobalVLMax + 1
  const bool IsForVSETIVLI;

  const PrimaryConfigMapping Mapping;

  // Note that we often have quite sparse distributions where >80% of
  // weights are 0.0. The std::discrete_distribution is not optimized for
  // such cases.
  mutable std::discrete_distribution<unsigned> Dist;

  RVVPrimaryConfigGenerator(
      unsigned ELEN, unsigned VLEN, const SewLmulDistribution &SewLmulDist,
      const ProbableItems<VLGeneratorHolder> &VLGenerators,
      unsigned MinVMBitWidth, bool IsForVSETIVLI);

  RVVPrimaryConfig idxToConfig(size_t Idx) const {
    return Mapping.idxToConfig(Idx);
  }
  size_t toIdx(VSEW SEW, VLMUL LMUL, unsigned VL) const {
    return Mapping.toIdx(SEW, LMUL, VL);
  }

  RVVPrimaryConfig generate() const;

  [[nodiscard]] PrimaryWeightsAndMapping getWeightsAndMapping() const {
    return {Dist.probabilities(), Mapping};
  }
  [[nodiscard]] ProbableItems<RVVPrimaryConfig> getAllPossibleConfigs() const {
    return getWeightsAndMapping().getAllPossibleConfigs();
  }
  [[nodiscard]] SewLmulDistribution getAllPossibleSewLmulPairs() const {
    return getWeightsAndMapping().getAllPossibleSewLmulPairs();
  }
  void printProbabilities(raw_ostream &OS) const {
    getWeightsAndMapping().printProbabilities(OS);
  }
};

struct RVVConfigGenerator final {
  // One generator for (VSETVLI & VSETVL) and another for VSETIVLI.
  // At least one generator must be present.
  std::optional<RVVPrimaryConfigGenerator> PrimaryGen;
  std::optional<RVVPrimaryConfigGenerator> PrimaryGenReduced;
  DiscreteItemGenerator<VMAMode> VmaGen;
  DiscreteItemGenerator<VTAMode> VtaGen;
  DiscreteItemGenerator<VXRMMode> VxrmGen;

  RVVConfigGenerator(
      std::optional<RVVPrimaryConfigGenerator> &&PrimaryGen,
      std::optional<RVVPrimaryConfigGenerator> &&PrimaryGenReduced,
      const VMAInfo &VMA, const VTAInfo &VTA, const VXRMInfo &VXRM)
      : PrimaryGen(std::move(PrimaryGen)),
        PrimaryGenReduced(std::move(PrimaryGenReduced)), VmaGen(VMA),
        VtaGen(VTA), VxrmGen(VXRM) {}

  RVVConfiguration generate(bool MustUseReducedVL) const;

  // Makes a weighted sum of the distributions of PrimaryGen and
  // PrimaryGenReduced according to the probabilities of VSET opcodes.
  [[nodiscard]] PrimaryWeightsAndMapping
  getCombinedDistribution(const ModeChangeInfo &SwitchInfo) const;
};

struct RVVSupportInfo final {
  RVVUnitInfo OriginalVUInfo;
  std::vector<std::string> DiscardedVMNames;
  std::vector<std::string> DiscardedVLNames;

  RVVSupportInfo(const RVVUnitInfo &OriginalVUInfo,
                 std::vector<std::string> &&DiscardedVMNames,
                 std::vector<std::string> &&DiscardedVLNames)
      : OriginalVUInfo(OriginalVUInfo),
        DiscardedVMNames(std::move(DiscardedVMNames)),
        DiscardedVLNames(std::move(DiscardedVLNames)) {}
};

struct RVVGeneratorInfo final {
  using VMGenerator = DiscreteItemGenerator<VMGeneratorHolder>;

  RVVConfigGenerator CfgGen;
  VMGenerator VMGen;

  // Stored for diagnostics/printing; not used in generation
  RVVSupportInfo SupportInfo;

public:
  RVVGeneratorInfo(RVVConfigGenerator &&CfgGen,
                   ProbableItems<VMGeneratorHolder> &&VMGen,
                   RVVSupportInfo &&SupportInfo)
      : CfgGen(std::move(CfgGen)), VMGen(std::move(VMGen)),
        SupportInfo(std::move(SupportInfo)) {}
};

class RVVConfigurationInfo final {
public:
  static RVVConfigurationInfo buildConfiguration(const Config &Cfg,
                                                 unsigned ELEN, unsigned VLEN);

  unsigned getELEN() const { return ELEN; }
  unsigned getVLEN() const { return VLEN; }
  unsigned getVLENB() const { return getVLEN() / RISCV_CHAR_BIT; }

  bool isRVVEnabled() const {
    bool RVVEnabled = (getVLEN() != 0);
    assert(RVVEnabled == GenInfo.has_value());
    return RVVEnabled;
  }

  APInt selectVM(unsigned VL) const;
  RVVConfiguration selectConfiguration(bool MustUseReducedVL) const;

  bool isModeChangeArtificial() const { return ArtificialModeChange; }
  bool isVXRMUpdateNeeded() const { return NeedsVXRMUpdate; }
  const ModeChangeInfo &getModeChangeInfo() const { return SwitchInfo; }

  ProbableItems<RVVPrimaryConfig> getAllPossiblePrimaryConfigs() const;

  void print(raw_ostream &OS) const;

  const RVVGeneratorInfo *getGenInfoPtr() const {
    return GenInfo ? &*GenInfo : nullptr;
  }

private:
  RVVConfigurationInfo(unsigned ELEN, unsigned VLEN,
                       ModeChangeInfo &&SwitchInfo, bool ArtificialModeChange,
                       bool NeedsVXRMUpdate,
                       std::optional<RVVGeneratorInfo> GenInfo = std::nullopt)
      : VLEN(VLEN), ELEN(ELEN), GenInfo(std::move(GenInfo)),
        SwitchInfo(std::move(SwitchInfo)),
        ArtificialModeChange(ArtificialModeChange),
        NeedsVXRMUpdate(NeedsVXRMUpdate) {}

  unsigned VLEN;
  unsigned ELEN;
  // Present only when VLEN > 0, doesn't make sense otherwise
  std::optional<RVVGeneratorInfo> GenInfo;
  ModeChangeInfo SwitchInfo;
  bool ArtificialModeChange;
  bool NeedsVXRMUpdate;
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
    unsigned ELEN = 0;
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
#endif // LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RVVUNITCONFIG_H
