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
#include "snippy/Generator/RootRegPoolWrapperPass.h"
#include "snippy/Generator/SMCInitPass.h"
#include "snippy/Generator/SMCManager.h"

#include "llvm/CodeGen/MachineModuleInfo.h"

#define DEBUG_TYPE "snippy-smc-memcpy"
#define PASS_DESC "Snippy SMC Memcpy Generator"

namespace llvm {
namespace snippy {
namespace {

class SMCGenerator final : public ModulePass {
  MachineModuleInfo *MMI = nullptr;

public:
  static char ID;

  SMCGenerator() : ModulePass(ID) {}
  SMCGenerator(MachineModuleInfo &MMI) : ModulePass(ID), MMI{&MMI} {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SMCInit>();
    AU.addRequired<RootRegPoolWrapper>();
  }
};

char SMCGenerator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SMCGenerator;

INITIALIZE_PASS(SMCGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createSMCGeneratorPass(MachineModuleInfo &MMI) {
  return new SMCGenerator(MMI);
}

namespace snippy {

bool SMCGenerator::runOnModule(Module &M) {
  if (!getAnalysis<SMCInit>().getSMCSrcMF())
    return false;
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();

  auto &SnpTgt = State.getSnippyTarget();
  auto &F = State.createFunction(M, SMCManagerT::SMCCopyFuncName,
                                 /* SectionName */ "",
                                 Function::ExternalLinkage, M.getContext());
  auto &MF = State.createMachineFunctionFor(F, *MMI, M.getContext(),
                                            /* SetSection */ true);

  SnpTgt.generateMemCpyForSMC(MF, ProgCtx);
  auto CallRegs = ProgCtx.getSMCManager().getSMCRegList();
  auto RootPool = getAnalysis<RootRegPoolWrapper>().getPool();
  for (auto &&Reg : CallRegs)
    RootPool.addReserved(Reg, MF);

  // FIXME: It would be better to have a special section for ancillary functions
  // to prevent code layout deoptimizations

  auto RA = ProgCtx.getReturnAddress();
  RootPool.addReserved(RA, MF);
  auto &ExitBlock = MF.back();
  InstructionGenerationContext IGC{ExitBlock, ExitBlock.end(), GC};
  SnpTgt.generateReturn(IGC, RA);
  return false;
}
} // namespace snippy
} // namespace llvm
