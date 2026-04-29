//===-- SMCSetSizeGlobals.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "../InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SMCManager.h"
#include "snippy/Generator/SnippyModule.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineModuleInfo.h"

#define DEBUG_TYPE "snippy-set-size-globals"
#define PASS_DESC "Snippy SMC Set Size Globals"

namespace llvm {
namespace snippy {
namespace {

struct SMCSetSizeGlobals final : public ModulePass {
  static char ID;
  SMCSetSizeGlobals() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
  }
};

char SMCSetSizeGlobals::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SMCSetSizeGlobals;

INITIALIZE_PASS(SMCSetSizeGlobals, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createSMCSetSizeGlobalsPass() { return new SMCSetSizeGlobals(); }

namespace snippy {

bool SMCSetSizeGlobals::runOnModule(Module &M) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();

  auto &GP =
      ProgCtx.getOrAddGlobalsPoolFor(M, "Failed to allocate space for SMC");
  auto &SMCManager = ProgCtx.getSMCManager();

  const auto &SMCBlockPairs = SMCManager.getSMCBlockPairs();
  std::map<std::string, const MachineBasicBlock *> UniquePairs(
      SMCBlockPairs.begin(), SMCBlockPairs.end());

  for (auto *TBB :
       map_range(UniquePairs, [](const auto &Pair) { return Pair.second; })) {
    auto AddrLen = Tgt.getAddrRegLen(TM);
    auto *Type = IntegerType::get(State.getCtx(), AddrLen);
    assert(TBB);
    auto GVSizeName =
        SMCManagerT::SMCTgtBlockSizePrefix.str() + getMBBSectionName(*TBB);
    auto *Size = ConstantInt::get(
        Type, State.getCodeBlockSize(TBB->begin(), TBB->getFirstTerminator()));

    auto *GVSize = GP.getGV(GVSizeName);
    GVSize->setInitializer(Size);
  }

  return false;
}
} // namespace snippy
} // namespace llvm
