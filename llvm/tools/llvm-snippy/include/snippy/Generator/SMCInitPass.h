//===-- SMCInitPass.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SMCINITPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SMCINITPASS_H

#include "llvm/Pass.h"

namespace llvm {

class MachineModuleInfo;
class MachineFunction;

namespace snippy {

class SMCInit final : public ModulePass {
  MachineModuleInfo *MMI = nullptr;
  MachineFunction *SMCSrcMF = nullptr;

public:
  static char ID;

  SMCInit() : ModulePass(ID) {}
  SMCInit(MachineModuleInfo &MMI) : ModulePass(ID), MMI{&MMI} {}

  StringRef getPassName() const override;

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  MachineFunction *getSMCSrcMF() { return SMCSrcMF; }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SMCINITPASS_H
