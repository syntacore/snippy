#ifndef LLVM_TOOLS_SNIPPY_LIB_RISCV_GEN_CONTEXT_H
#define LLVM_TOOLS_SNIPPY_LIB_RISCV_GEN_CONTEXT_H

//===-- TargetGenContext.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"

#include "RVVUnitConfig.h"

#include <optional>

#include "RISCVGenerated.h"

namespace llvm {
namespace snippy {

struct RVVModeInfo {
  RVVConfigurationInfo::VLVM VLVM;
  const RVVConfiguration *Config = nullptr;
  const MachineBasicBlock *MBBGuard = nullptr;
};

struct VSETWeightOverrides {
  VSETWeightOverrides() {
    setVSETVLWeight(OpcodeHistogramEntry::IgnoredWeight);
    setVSETVLIWeight(OpcodeHistogramEntry::IgnoredWeight);
    setVSETIVLIWeight(OpcodeHistogramEntry::IgnoredWeight);
  }

  void setVSETVLWeight(double W) { Result[0] = {RISCV::VSETVL, W}; }
  void setVSETVLIWeight(double W) { Result[1] = {RISCV::VSETVLI, W}; }
  void setVSETIVLIWeight(double W) { Result[2] = {RISCV::VSETIVLI, W}; }

  const auto &getEntries() const { return Result; }

private:
  std::array<OpcodeHistogramEntry, 3> Result;
};

class RISCVGeneratorContext : public TargetGenContextInterface {
public:
  RISCVGeneratorContext(RISCVConfigurationInfo &&RISCVConfigIn)
      : RISCVConfig(std::move(RISCVConfigIn)) {}

  const RVVConfigurationInfo &getVUConfigInfo() const {
    return RISCVConfig.getVUConfig();
  }
  const BaseConfigurationInfo &getBaseConfigInfo() const {
    return RISCVConfig.getBaseConfig();
  }

  const RVVModeInfo &getActiveRVVMode(const MachineBasicBlock &MBB) const {
    // FIXME: this is kind of non re-entrant (meaning we have a potential
    // issue in case if RISCVGeneratorContext is reused)
    assert(hasActiveRVVMode(MBB));
    return CurrentRVVMode;
  }

  VSETWeightOverrides getVSETOverrides(const MachineBasicBlock &MBB) const {
    // Note: by default all entries are marked as ignored
    VSETWeightOverrides Result;
    // VSET* weights are overriden if we have a bias-based mode changing
    // scheme or if no pending RVV mode is selected
    // See ModeChangeInfo type definition for details
    if (hasActiveRVVMode(MBB) && !getVUConfigInfo().isModeChangeArtificial())
      return Result;

    const auto &ModeChangeInfo = getVUConfigInfo().getModeChangeInfo();
    if (!ModeChangeInfo.RVVPresent)
      return Result;

    Result.setVSETVLWeight(ModeChangeInfo.WeightVSETVL);
    Result.setVSETVLIWeight(ModeChangeInfo.WeightVSETVLI);
    Result.setVSETIVLIWeight(ModeChangeInfo.WeightVSETIVLI);

    if (!hasActiveRVVMode(MBB) && getVUConfigInfo().isModeChangeArtificial() &&
        all_of(Result.getEntries(),
               [](const auto &E) { return E.Weight == 0; })) {
      // Mode change prob is zero, but we must initialize RVV unit.
      // FIXME: Constant of 1.0 is not the best choice. It must likely depend on
      // other rvv instructions weights.
      Result.setVSETVLWeight(1.0);
      Result.setVSETVLIWeight(1.0);
      Result.setVSETIVLIWeight(1.0);
    }
    return Result;
  }

  bool hasActiveRVVMode(const MachineBasicBlock &MBB) const {
    return CurrentRVVMode.MBBGuard == &MBB;
  }

  auto getVLENB() const { return getVUConfigInfo().getVLENB(); }

  auto getVLEN() const { return getVUConfigInfo().getVLEN(); }

  auto getLMUL(const MachineBasicBlock &MBB) const {
    return getActiveRVVMode(MBB).Config->LMUL;
  }

  auto getSEW(const MachineBasicBlock &MBB) const {
    return getActiveRVVMode(MBB).Config->SEW;
  }

  std::pair<unsigned, bool> getEMUL(unsigned Opcode, unsigned OpIndex,
                                    const MachineBasicBlock &MBB) const {
    // EMUL - effective LMUL of each vector operand.

    // Vector unit-stride and constant-stride use the EEW/EMUL encoded
    // in the instruction for the data values.
    //
    // E.G. (Unit-Stride):
    //
    //           vle16.v vd, (rs1), vm          # 16-bit unit-stride load
    //           vsse32.v vs3, (rs1), rs2, vm   # 32-bit strided store
    //
    // E.G. (Strided):
    //
    //           vlseg8e8.v vd, (rs1), vm
    //                                          # Load eight vector registers
    //                                          with eight byte fields.
    //
    // Conclusion: EMUL of destination vector register vd, must be calculated.
    // There are no more vector operands in this instruction (mask does not
    // require EMUL calculation), so there is an assertion.
    if (isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
        isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedLoadStore(Opcode) ||
        isRVVStridedSegLoadStore(Opcode)) {
      assert(OpIndex == 0 && "We can be here only for vd vector operand");
      auto LMUL = getLMUL(MBB);
      auto SEW = getSEW(MBB);
      auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
      return computeDecodedEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
    }

    // For Vector Indexed Loads and Stores Instructions EMULs of the operands
    // are different.
    //
    // E.G. (Indexed):
    //
    //          vluxei32.v vd, (rs1), vs2, vm
    //                                         # unordered 32-bit
    //                                         indexed load of SEW data
    //
    // The destination vector register group (vd) has EEW = SEW,
    // EMUL = LMUL = 1, while the index vector register group (vs2) has
    // EEW encoded in the instruction (32) with
    // EMUL = (32 / SEW) * LMUL.
    //
    // E.G. (Segment Indexed):
    //
    // Format:  vluxseg<nf>ei<eew>.v vd, (rs1), vs2, vm
    //
    //          vsetvli a1, t0, e8, ta, ma
    //          vluxseg3ei32.v v4, (x5), v3
    //
    //                                         # Load bytes at
    //                                         addresses x5+v3[i] into v4[i],
    //                                         addresses x5+v3[i]+1 into v5[i],
    //                                         addresses x5+v3[i]+2 into v6[i].
    //
    // The destination vector register group (vd, i.e v4) has EEW = SEW = 8,
    // EMUL = LMUL = 1, while the index vector register group (vs2, i.e. v3) has
    // EEW encoded in the instruction (32) with
    // EMUL = (EEW / SEW) * LMUL = (32 / 8) * 1 = 4.

    // Conclusion: EMUL of index vector register, which are specified by the
    // third operand (OpIndex == 2), must be calculated. While for another
    // vector operand EMUL is simply equal to the LMUL.
    auto [Multiplier, IsFractional] = decodeVLMUL(getLMUL(MBB));
    if (isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) {
      if (OpIndex == 0)
        return std::make_pair(Multiplier, IsFractional);
      assert(OpIndex == 2 && "We can be here only for index vector operand");
      auto LMUL = getLMUL(MBB);
      auto SEW = static_cast<unsigned>(getSEW(MBB));
      auto EIEW = getIndexElementWidth(Opcode);
      return computeDecodedEMUL(static_cast<unsigned>(SEW), EIEW, LMUL);
    }

    // FIXME: This function must take into account the index of the operand
    // in this instructions too. Now it returns the maximum EMUL of all
    // operands.
    //
    // For Widening Vector Arithmetic Instructions EMULs of the operands
    // are also different.
    //
    // E.G.:
    //        # Double-width result, two single-width sources:
    //        2*SEW = SEW op SEW
    //
    //        vwop.vv vd, vs2, vs1, vm       # integer vector-vector
    //                                       vd[i] = vs2[i] op vs1[i]
    //
    // E.G.:
    //        # Double-width result, first source double, second - single-width:
    //        2*SEW = SEW op SEW
    //
    //        vwop.vv vd, vs2, vs1, vm       # integer vector-vector
    //                                       vd[i] = vs2[i] op vs1[i]

    // For Narrowing Vector Arithmetic Instructions too
    //
    // E.G.:
    //        # Single-width result vd, double-width source vs2,
    //        # single-width source vs1/rs1: SEW = 2*SEW op SEW
    //
    //        vnop.wv vd, vs2, vs1, vm       # integer vector-vector
    //                                       vd[i] = vs2[i] op vs1[i]
    //
    if ((isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
         isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) &&
        !IsFractional) {
      auto LMUL = getLMUL(MBB);
      auto SEW = static_cast<unsigned>(getSEW(MBB));
      return computeDecodedEMUL(SEW, SEW * 2u, LMUL);
    }

    if (isRVVGather16(Opcode)) {
      auto LMUL = getLMUL(MBB);
      auto SEW = static_cast<unsigned>(getSEW(MBB));
      auto EEW = 16u;
      if (EEW > SEW)
        std::tie(Multiplier, IsFractional) = computeDecodedEMUL(SEW, EEW, LMUL);
    }

    return std::make_pair(Multiplier, IsFractional);
  }

  // This function differs from the RISCVVType::decodeVLMUL in that it also
  // handles the reserved LMUL
  static std::pair<unsigned, bool> decodeVLMUL(RISCVII::VLMUL LMUL) {
    if (LMUL == RISCVII::VLMUL::LMUL_RESERVED)
      return std::make_pair(1 << static_cast<unsigned>(LMUL), false);
    return RISCVVType::decodeVLMUL(LMUL);
  }

  const RVVConfiguration &getCurrentRVVCfg(const MachineBasicBlock &MBB) const {
    return *getActiveRVVMode(MBB).Config;
  }

  auto getVL(const MachineBasicBlock &MBB) const {
    return getActiveRVVMode(MBB).VLVM.VL;
  }

  void updateActiveRVVMode(RVVConfigurationInfo::VLVM VLVM,
                           const RVVConfiguration &RVVCfg,
                           const MachineBasicBlock &MBB) {
    CurrentRVVMode.VLVM = std::move(VLVM);
    CurrentRVVMode.Config = &RVVCfg;
    CurrentRVVMode.MBBGuard = &MBB;
  }

private:
  RVVModeInfo CurrentRVVMode;
  RISCVConfigurationInfo RISCVConfig;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_RISCV_GEN_CONTEXT_H
