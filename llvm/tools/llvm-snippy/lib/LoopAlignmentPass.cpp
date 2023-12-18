//===-- LoopAlignmentPass.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// LoopAlignment pass implementation. This pass aligns loops with value given
/// in branchegram.
///
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/PassRegistry.h"

#define DEBUG_TYPE "snippy-loop-alignment"
#define PASS_DESC "Snippy Loop Alignment"

namespace llvm {
namespace snippy {
namespace {

struct LoopAlignment final : public MachineFunctionPass {
  static char ID;

  LoopAlignment();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char LoopAlignment::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LoopAlignment;

INITIALIZE_PASS(LoopAlignment, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createLoopAlignmentPass() { return new LoopAlignment(); }

namespace snippy {

LoopAlignment::LoopAlignment() : MachineFunctionPass(ID) {
  initializeLoopAlignmentPass(*PassRegistry::getPassRegistry());
}

bool LoopAlignment::runOnMachineFunction(MachineFunction &MF) {
  Align Alignment(getAnalysis<GeneratorContextWrapper>()
                      .getContext()
                      .getGenSettings()
                      .Cfg.Branches.Alignment);

  auto &MLI = getAnalysis<MachineLoopInfo>();
  for (auto *ML : MLI) {
    auto *Header = ML->getHeader();
    assert(Header && "Loop must have header");
    Header->setAlignment(Alignment, 0);
    LLVM_DEBUG(dbgs() << "Aligned "; Header->dump());
  }

  return !MLI.empty();
}

} // namespace snippy
} // namespace llvm
