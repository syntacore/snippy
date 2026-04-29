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
#include "snippy/Generator/SMCInitPass.h"
#include "snippy/Generator/SMCManager.h"

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/InlineAsm.h"

#define DEBUG_TYPE "snippy-smc-bb-to-name"
#define PASS_DESC "Snippy SMC BB To Name"

namespace llvm {
namespace snippy {
namespace {

class SMCBBToName final : public ModulePass {
public:
  static char ID;

  SMCBBToName() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SMCInit>();
  }
};

char SMCBBToName::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SMCBBToName;

INITIALIZE_PASS(SMCBBToName, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createSMCBBToNamePass() { return new SMCBBToName(); }

namespace snippy {

bool SMCBBToName::runOnModule(Module &M) {
  auto *SMCSrcMF = getAnalysis<SMCInit>().getSMCSrcMF();
  if (!SMCSrcMF)
    return false;
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();

  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();

  assert(SMCSrcMF->size());

  auto &GP =
      ProgCtx.getOrAddGlobalsPoolFor(M, "Failed to get GP for SMCModule");
  auto &SMCManager = ProgCtx.getSMCManager();
  for (auto &&[MBB, Name] : zip(llvm::drop_begin(*SMCSrcMF),
                                SMCManager.getSrcNamesFromSMCBlockPairs())) {
    auto *DummyGV = GP.createGV(APInt::getZero(Tgt.getAddrRegLen(TM)), 1,
                                GlobalValue::ExternalLinkage, Name);
    SMCManager.addToSMCSrcMap(&MBB, DummyGV);
  }
  return false;
}
} // namespace snippy
} // namespace llvm
