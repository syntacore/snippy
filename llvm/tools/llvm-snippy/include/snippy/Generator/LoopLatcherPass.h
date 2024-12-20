//===-- LoopLatcherPass.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/ActiveImmutablePass.h"
#include "snippy/Generator/SnippyLoopInfo.h"

namespace llvm {
namespace snippy {

class LoopLatcher final
    : public ActiveImmutablePass<MachineFunctionPass, SnippyLoopInfo> {
public:
  static char ID;
  LoopLatcher();

  StringRef getPassName() const override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void processExitingBlock(MachineLoop &ML, MachineBasicBlock &ExitingBlock,
                           MachineBasicBlock &Preheader);
  bool createLoopLatchFor(MachineLoop &ML);
  template <typename R>
  bool createLoopLatchFor(MachineLoop &ML, R &&ConsecutiveLoops);
  auto selectRegsForBranch(const MCInstrDesc &BranchDesc,
                           const MachineBasicBlock &Preheader,
                           const MachineBasicBlock &ExitingBlock,
                           const MCRegisterClass &RegClass);
  MachineInstr &updateLatchBranch(MachineLoop &ML, MachineInstr &Branch,
                                  MachineBasicBlock &Preheader,
                                  ArrayRef<Register> ReservedRegs);

  bool NIterWarned = false;
};

} // namespace snippy
} // namespace llvm
