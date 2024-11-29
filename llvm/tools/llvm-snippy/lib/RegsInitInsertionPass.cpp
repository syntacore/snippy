//===-- RegsInitInsertionPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Generator/SnippyFunctionMetadata.h"

#include "llvm/CodeGen/MachineFunctionPass.h"

#define DEBUG_TYPE "snippy-regs-init-insertion"
#define PASS_DESC "Snippy Registers Initialization Insertion"

namespace llvm {
namespace snippy {
namespace {

struct RegsInitInsertion final : public MachineFunctionPass {
  bool InitRegs;

public:
  static char ID;

  RegsInitInsertion(bool InitRegs = true)
      : MachineFunctionPass(ID), InitRegs(InitRegs) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SnippyFunctionMetadataWrapper>();
    AU.addRequired<FunctionGenerator>();
    AU.addRequired<SimulatorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char RegsInitInsertion::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::RegsInitInsertion;

INITIALIZE_PASS(RegsInitInsertion, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createRegsInitInsertionPass(bool InitRegs) {
  return new RegsInitInsertion(InitRegs);
}

namespace snippy {

bool RegsInitInsertion::runOnMachineFunction(MachineFunction &MF) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &FG = getAnalysis<FunctionGenerator>();
  auto &SFM = getAnalysis<SnippyFunctionMetadataWrapper>().get(MF);
  auto &SimCtx =
      getAnalysis<SimulatorContextWrapper>().get<OwningSimulatorContext>();
  if (!FG.isEntryFunction(MF))
    return false;
  if (!InitRegs) {
    MF.getRegInfo().invalidateLiveness();
    return false;
  }
  auto &State = SGCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &SubTgt = MF.getSubtarget();

  // new block for registers initialization
  auto *BlockRegsInit = createMachineBasicBlock(MF, SGCtx);
  auto *SuccessorBlockPtr = &MF.front();
  auto InsertIterPos = MF.begin();
  BlockRegsInit->addSuccessor(SuccessorBlockPtr);
  MF.insert(InsertIterPos, BlockRegsInit);
  SFM.RegsInitBlock = BlockRegsInit;

  planning::InstructionGenerationContext IGC{
      *BlockRegsInit, BlockRegsInit->getFirstTerminator(), SGCtx, SimCtx};

  SnippyTgt.generateRegsInit(
      IGC, SGCtx.getProgramContext().getInitialRegisterState(SubTgt));
  return true;
}

} // namespace snippy
} // namespace llvm
