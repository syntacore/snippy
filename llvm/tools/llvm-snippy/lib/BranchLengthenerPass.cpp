//===-- JumpLengthenerPass.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "InitializePasses.h"

#include "llvm/CodeGen/MachineFunctionPass.h"

#define DEBUG_TYPE "snippy-branch-lengthener"
#define PASS_DESC "Snippy Branch Lengthener"

namespace llvm {
namespace snippy {
namespace {
class BranchLengthener final : public MachineFunctionPass {
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB) const;

public:
  static char ID;

  BranchLengthener() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace
char BranchLengthener::ID = 0;
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::BranchLengthener;

INITIALIZE_PASS_BEGIN(BranchLengthener, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(BranchLengthener, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {
namespace snippy {

StringRef BranchLengthener::getPassName() const { return PASS_DESC " Pass"; }

void BranchLengthener::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

static void lengthenBranch(MachineInstr &Br, MachineBasicBlock &MBB,
                           GeneratorContext &GC, const SnippyTarget &Tgt) {
  auto *BrDest = Tgt.getBranchDestination(Br);
  assert(BrDest);
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  Tgt.insertFallbackBranch(MBB, *BrDest, State);
  auto &InsertedJump = MBB.back();

  auto *Symbol =
      State.getMCContext().getOrCreateSymbol(getMBBSectionName(MBB) + ".dest");
  assert(Symbol);
  auto *MFPtr = MBB.getParent();
  assert(MFPtr);
  auto &MF = *MFPtr;
  InsertedJump.setPreInstrSymbol(MF, Symbol);
  Tgt.replaceBranchDest(Br, InsertedJump);
}

bool BranchLengthener::runOnMachineBasicBlock(MachineBasicBlock &MBB) const {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto FirstTermIt = MBB.getFirstTerminator();
  if (FirstTermIt == MBB.end())
    return false;
  auto &FirstTerm = *FirstTermIt;
  if (!FirstTerm.isConditionalBranch())
    return false;
  lengthenBranch(FirstTerm, MBB, GC, Tgt);
  return true;
}

bool BranchLengthener::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  for (auto &MBB : MF)
    Changed |= runOnMachineBasicBlock(MBB);
  return Changed;
}

} // namespace snippy

MachineFunctionPass *createBranchLengthenerPass() {
  return new snippy::BranchLengthener;
}

} // namespace llvm
