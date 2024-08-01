//===-- CFGPrinterPass.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/PassRegistry.h"

#define DEBUG_TYPE "snippy-cfg-printer"
#define PASS_DESC "Snippy CFG Printer"

namespace llvm {
namespace snippy {
namespace {
struct CFGPrinter final : public MachineFunctionPass {
  static char ID;

  CFGPrinter() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    MF.viewCFG();
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char CFGPrinter::ID = 0;
} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CFGPrinter;

INITIALIZE_PASS(CFGPrinter, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {
MachineFunctionPass *createCFGPrinterPass() { return new CFGPrinter(); }
} // namespace llvm
