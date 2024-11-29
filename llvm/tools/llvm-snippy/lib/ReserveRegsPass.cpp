//===-- ReserveRegsPass.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/RootRegPoolWrapperPass.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"

#define DEBUG_TYPE "snippy-register-reserve"
#define PASS_DESC "Snippy Register Reserve"

namespace llvm {
namespace snippy {
namespace {

struct ReserveRegs final : public ModulePass {
  static char ID;

  ReserveRegs() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SimulatorContextWrapper>();
    AU.addRequired<RootRegPoolWrapper>();
    ModulePass::getAnalysisUsage(AU);
  }
};

char ReserveRegs::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::ReserveRegs;

INITIALIZE_PASS_BEGIN(ReserveRegs, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(RootRegPoolWrapper)
INITIALIZE_PASS_END(ReserveRegs, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createReserveRegsPass() { return new ReserveRegs(); }

namespace snippy {

bool ReserveRegs::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  const auto &ProgCtx = SGCtx.getProgramContext();

  if (!ProgCtx.stackEnabled())
    return false;

  auto StackPointer = ProgCtx.getStackPointer();
  auto &RootPool = getAnalysis<RootRegPoolWrapper>().getPool();

  // When stack enabled, it is not allow to modify stack pointer in any way.
  // Additionally, if we use model during instructions generation, we must
  // disallow reading of SP since its value is not known at this stage.
  auto ReservationMode =
      SimCtx.hasTrackingMode() ? AccessMaskBit::RW : AccessMaskBit::W;
  RootPool.addReserved(StackPointer, ReservationMode);

  return true;
}

} // namespace snippy
} // namespace llvm
