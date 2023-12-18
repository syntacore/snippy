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
    auto VLen = getVUConfigInfo().getVLEN();
  }

private:
  RVVModeInfo CurrentRVVMode;
  RISCVConfigurationInfo RISCVConfig;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_RISCV_GEN_CONTEXT_H
