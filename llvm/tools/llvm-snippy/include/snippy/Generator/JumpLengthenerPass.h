//===-- JumpLengthenerPass.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_JUMPLENGTHENERPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_JUMPLENGTHENERPASS_H

#include "snippy/ActiveImmutablePass.h"
#include "snippy/Generator/IndJumpInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
namespace snippy {

class JumpLengthener final
    : public ActiveImmutablePass<MachineFunctionPass, IndJumpInfoMap> {
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB, IndJumpInfoMap &JumpMap);

public:
  static char ID;

  JumpLengthener() : ActiveImmutablePass(ID) {}

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_JUMPLENGTHENERPASS_H
