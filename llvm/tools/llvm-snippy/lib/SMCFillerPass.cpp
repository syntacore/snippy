//===-- SMCGeneratorPass.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/Generation.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SMCInitPass.h"
#include "snippy/Generator/SMCManager.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineModuleInfo.h"

#define DEBUG_TYPE "snippy-smc-filler"
#define PASS_DESC "Snippy SMC Filler Generator"

namespace llvm {
namespace snippy {
namespace {

class SMCFiller final : public ModulePass {
  LLVMState *State = nullptr;

public:
  static char ID;

  SMCFiller() : ModulePass(ID) {}
  SMCFiller(LLVMState &State) : ModulePass(ID), State{&State} {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SimulatorContextWrapper>();
    AU.addRequired<SMCInit>();
  }
};

char SMCFiller::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SMCFiller;

INITIALIZE_PASS(SMCFiller, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createSMCFillerPass(snippy::LLVMState &State) {
  return new SMCFiller(State);
}

namespace snippy {

bool SMCFiller::runOnModule(Module &M) {
  auto *SMCSrcMF = getAnalysis<SMCInit>().getSMCSrcMF();
  if (!SMCSrcMF)
    return false;
  auto &MF = *SMCSrcMF;
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &SimCtx = getAnalysis<SimulatorContextWrapper>()
                     .get<OwningSimulatorContext>()
                     .get();
  auto &ProgCtx = GC.getProgramContext();

  assert(MF.size() && "empty SMC Source MF");

  planning::FunctionRequest FunReq(MF, GC);
  size_t BlockSize = 0;
  auto Limit = planning::RequestLimit::Size{0};
  auto Policy =
      planning::createGenPolicy(ProgCtx, GC.getConfig().DefFlowConfig);

  FunReq.addToBlock(&(MF.front()), planning::InstructionGroupRequest(
                                       Limit, std::move(Policy)));
  auto &SMCManager = ProgCtx.getSMCManager();
  for (auto &&[MBB, TBB] :
       zip(drop_begin(MF), SMCManager.getTgtBlocksFromBlockPairs())) {
    BlockSize =
        State->getCodeBlockSize(TBB->begin(), TBB->getFirstTerminator());
    Limit = planning::RequestLimit::Size{BlockSize};
    Policy = planning::createGenPolicy(ProgCtx, GC.getConfig().DefFlowConfig);
    FunReq.addToBlock(
        &MBB, planning::InstructionGroupRequest(Limit, std::move(Policy)));
  }

  generate(FunReq, MF, GC, SimCtx);

  return false;
}
} // namespace snippy
} // namespace llvm
