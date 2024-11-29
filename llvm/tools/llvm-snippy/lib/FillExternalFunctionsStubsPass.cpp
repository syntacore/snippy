//===-- FillExternalFunctionsStubsPass.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/Generator/GenerationUtils.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"

#define DEBUG_TYPE "snippy-fill-external-functions-stubs"
#define PASS_DESC "Snippy Fill External Functions Stubs"

namespace llvm {
namespace snippy {
namespace {

class FillExternalFunctionsStubs final : public ModulePass {
  const std::vector<std::string> FunctionsToAvoid;

public:
  static char ID;

  FillExternalFunctionsStubs() : ModulePass(ID) {}

  FillExternalFunctionsStubs(const std::vector<std::string> &FunctionsToAvoid)
      : ModulePass(ID), FunctionsToAvoid{FunctionsToAvoid} {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    ModulePass::getAnalysisUsage(AU);
  }
};

char FillExternalFunctionsStubs::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::FillExternalFunctionsStubs;

INITIALIZE_PASS_BEGIN(FillExternalFunctionsStubs, DEBUG_TYPE, PASS_DESC, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(FillExternalFunctionsStubs, DEBUG_TYPE, PASS_DESC, false,
                    false)

namespace llvm {

ModulePass *createFillExternalFunctionsStubsPass(
    const std::vector<std::string> &FunctionsToAvoid) {
  return new FillExternalFunctionsStubs(FunctionsToAvoid);
}

namespace snippy {

bool FillExternalFunctionsStubs::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  for (auto &F : M) {
    // 'External' function must have no body.
    if (!F.empty() ||
        std::any_of(
            FunctionsToAvoid.begin(), FunctionsToAvoid.end(),
            [&F](StringRef FuncName) { return F.getName() == FuncName; }))
      continue;

    auto &MF =
        State.createMachineFunctionFor(F, SGCtx.getMainModule().getMMI());
    auto *MBB = createMachineBasicBlock(MF, SGCtx);
    MF.push_back(MBB);
    InstructionGenerationContext IGC{*MBB, MBB->end(), SGCtx};
    SnippyTgt.generateReturn(IGC);
  }

  return true;
}

} // namespace snippy
} // namespace llvm
