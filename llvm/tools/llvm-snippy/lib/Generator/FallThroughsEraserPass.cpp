//===-- FallThroughEraserPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/FallThroughsEraserPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"

namespace llvm {

MachineFunctionPass *createFallThroughEraserPass() {
  return new snippy::FallThroughEraserPass;
}

namespace snippy {
char FallThroughEraserPass::ID = 0;

void FallThroughEraserPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool FallThroughEraserPass::runOnMachineFunction(MachineFunction &MF) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  bool Changed = false;
  for (auto &MBB : MF) {
    auto *FallThrough = MBB.getFallThrough(/* JumpToFallThrough */ false);
    if (!FallThrough)
      continue;
    Tgt.generateJump(MBB, MBB.end(), *FallThrough, ProgCtx.getLLVMState());
    Changed = true;
  }
  return Changed;
}
} // namespace snippy
} // namespace llvm
