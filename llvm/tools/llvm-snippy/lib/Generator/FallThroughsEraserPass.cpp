//===-- FallThroughsEraserPass.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/FallThroughsEraserPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "../InitializePasses.h"

#define DEBUG_TYPE "snippy-fallthrough-eraser"
#define PASS_DESC "Snippy Fallthrough Eraser"

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::FallThroughsEraser;

INITIALIZE_PASS_BEGIN(FallThroughsEraser, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(FallThroughsEraser, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {
StringRef FallThroughsEraser::getPassName() const { return PASS_DESC; }

MachineFunctionPass *createFallThroughsEraserPass() {
  return new snippy::FallThroughsEraser;
}

namespace snippy {
char FallThroughsEraser::ID = 0;

void FallThroughsEraser::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool FallThroughsEraser::runOnMachineFunction(MachineFunction &MF) {
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
