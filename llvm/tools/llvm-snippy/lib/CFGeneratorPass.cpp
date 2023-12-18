//===-- CFGeneratorPass.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Generator/LLVMState.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/PassRegistry.h"

#define DEBUG_TYPE "snippy-cf-generator"
#define PASS_DESC "Snippy Control Flow Generator"

namespace llvm {
namespace snippy {
namespace {

struct CFGenerator final : public MachineFunctionPass {
  static char ID;

  CFGenerator();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char CFGenerator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CFGenerator;

INITIALIZE_PASS(CFGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createCFGeneratorPass() { return new CFGenerator(); }

namespace snippy {

CFGenerator::CFGenerator() : MachineFunctionPass(ID) {
  initializeCFGeneratorPass(*PassRegistry::getPassRegistry());
}

bool CFGenerator::runOnMachineFunction(MachineFunction &MF) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto CFInstrsNum = SGCtx.getCFInstrsNum(MF);
  if (CFInstrsNum == 0)
    return false;

  auto &State = SGCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto CFOpcGen = SGCtx.createCFOpcodeGenerator();
  auto *CurrMBB = &MF.front();
  for (auto NInstr = 0u; NInstr < CFInstrsNum; ++NInstr) {
    auto Opc = CFOpcGen->generate();
    const auto &InstrDesc = InstrInfo.get(Opc);
    CurrMBB = SnippyTgt.generateBranch(InstrDesc, *CurrMBB, SGCtx);
  }

  return true;
}

} // namespace snippy
} // namespace llvm
