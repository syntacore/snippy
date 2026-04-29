//===-- SMCGeneratorPass.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SMCInitPass.h"
#include "snippy/Generator/SMCManager.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "snippy-smc-initializer"
#define PASS_DESC "Snippy SMC Initializer Generator"

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SMCInit;

INITIALIZE_PASS(SMCInit, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createSMCInitPass(MachineModuleInfo &MMI) {
  return new SMCInit(MMI);
}

namespace snippy {

char SMCInit::ID = 0;

StringRef SMCInit::getPassName() const { return PASS_DESC " Pass"; }

void SMCInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<GeneratorContextWrapper>();
}

bool SMCInit::runOnModule(Module &M) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  const auto &State = ProgCtx.getLLVMState();
  auto &SMCManager = ProgCtx.getSMCManager();

  const auto &SMCBlockPairs = SMCManager.getSMCBlockPairs();
  if (!SMCBlockPairs.size()) {
    snippy::warn(
        WarningName::SMCWithoutEffect, "SMC mode doesn't provide any effect",
        "there are no branches in the histogram or their weight is too small");
    return false;
  }

  auto &F = State.createFunction(M, SMCManagerT::SMCSrcFuncName,
                                 /* SectionName */ "",
                                 Function::ExternalLinkage, M.getContext());
  auto &MF = State.createMachineFunctionFor(F, *MMI, M.getContext(),
                                            /* SetSection */ true);

  auto *EntryMBB = snippy::createMachineBasicBlock(MF);
  MF.push_back(EntryMBB);

  const auto &InstrInfo = State.getInstrInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto CFOpcGen = GC.getConfig().PassCfg.createCFOpcodeGenerator();

  auto *CurrMBB = EntryMBB;
  for (auto NInstr = 0u; NInstr < SMCBlockPairs.size(); ++NInstr) {
    auto CFOpc = generateSingleOpcode(*CFOpcGen);
    const auto &InstrDesc = InstrInfo.get(CFOpc);
    InstructionGenerationContext IGC{*CurrMBB, CurrMBB->getFirstTerminator(),
                                     GC};
    CurrMBB = SnippyTgt.generateBranch(IGC, InstrDesc);
  }

  SMCSrcMF = &MF;

  return true;
}
} // namespace snippy
} // namespace llvm
