//===-- CLBasicBlockPreprocessPass.cpp --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../InitializePasses.h"

#include "snippy/CreatePasses.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "snippy-cl-bb-preprocess"
#define PASS_DESC "Snippy CL basic block preprocess"

namespace llvm {

namespace snippy {

namespace {

class CLBasicBlockPreprocess : public MachineFunctionPass {
public:
  static char ID;
  CLBasicBlockPreprocess() : MachineFunctionPass(ID) {};
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    for (auto &MBB : MF) {
      // This need to be done before first ever getSymbol()
      // is called for MBB, because symbol is lazily created
      // and then cached.
      // Currently flow generator pass may cause this method
      // to be called, so this pass should be run before it.
      MBB.setIsBeginSection();
      MBB.setLabelMustBeEmitted();
    }
    return true;
  }
};

char CLBasicBlockPreprocess::ID = 0;

} // namespace

} // namespace snippy

MachineFunctionPass *createCLBasicBlockPreprocessPass() {
  return new snippy::CLBasicBlockPreprocess;
}

} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CLBasicBlockPreprocess;

INITIALIZE_PASS(CLBasicBlockPreprocess, DEBUG_TYPE, PASS_DESC, false, false)
