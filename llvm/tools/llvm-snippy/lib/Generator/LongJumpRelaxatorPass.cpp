//===-- LongJumpRelaxatorPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../InitializePasses.h"

#include "snippy/Generator/CodeAddrSamplingPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/JumpLengthenerPass.h"

#include "llvm/CodeGen/MachineFunctionPass.h"

#define DEBUG_TYPE "snippy-long-jump-relaxator"
#define PASS_DESC "Snippy Long Jump Relaxator"

namespace llvm {
namespace snippy {

static void eraseJumpAndAssociates(MachineInstr &J, IndJumpInfoMap &JumpMap) {
  auto &[TBB, SupportInstrs] = JumpMap.getInfo(J);
  for (auto *I : SupportInstrs) {
    assert(I);
    I->eraseFromParent();
  }
  J.eraseFromParent();
  JumpMap.remove(J);
}

static MachineInstr &
replaceWithJump(MachineBasicBlock &MBB, MachineBasicBlock &TBB,
                const IndJumpInfo &Info, MachineBasicBlock::iterator J,
                GeneratorContext &GC, const SnippyTarget &Tgt,
                IndJumpInfoMap &JumpMap) {
  auto NewJumpIt =
      Tgt.generateJump(MBB, J, TBB, GC.getProgramContext().getLLVMState());
  auto *MF = MBB.getParent();
  assert(MF);
  NewJumpIt->cloneInstrSymbols(*MF, *J);
  eraseJumpAndAssociates(*J, JumpMap);
  return *NewJumpIt;
}

static void replaceWithBranch(MachineBasicBlock &MBB, const IndJumpInfo &Info,
                              MachineBasicBlock::iterator J,
                              GeneratorContext &GC, const SnippyTarget &Tgt,
                              IndJumpInfoMap &JumpMap) {
  auto Reversed = llvm::reverse(llvm::make_range(MBB.getFirstTerminator(), J));
  auto BrIt = llvm::find_if(
      Reversed, [](const auto &MI) { return MI.isConditionalBranch(); });
  auto &[TBB, SupportInstrs] = Info;
  assert(TBB);

  // Simply replace single indirect jump with jump
  if (BrIt == Reversed.end()) {
    replaceWithJump(MBB, *TBB, Info, J, GC, Tgt, JumpMap);
    return;
  }
  auto &Br = *BrIt;
  assert(Br.isConditionalBranch());
  bool IsFallback = std::next(BrIt.base()) == J;
  // Simply replace fallback jump
  if (!IsFallback) {
    replaceWithJump(MBB, *TBB, Info, J, GC, Tgt, JumpMap);
    return;
  }
  // Replace target jump and make it Br's destination
  auto &NewJump = replaceWithJump(MBB, *TBB, Info, J, GC, Tgt, JumpMap);
  assert(NewJump.getPreInstrSymbol());
  Tgt.replaceBranchDest(Br, NewJump);
  return;
}

namespace {
class LongJumpRelaxator final : public MachineFunctionPass {
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB,
                              IndJumpInfoMap &JumpMap) const;

public:
  static char ID;

  LongJumpRelaxator() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace
char LongJumpRelaxator::ID;

StringRef LongJumpRelaxator::getPassName() const { return PASS_DESC " Pass"; }
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LongJumpRelaxator;

INITIALIZE_PASS_BEGIN(LongJumpRelaxator, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(LongJumpRelaxator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createLongJumpRelaxatorPass() {
  return new snippy::LongJumpRelaxator;
}

namespace snippy {

void LongJumpRelaxator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<JumpLengthener>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool LongJumpRelaxator::runOnMachineBasicBlock(MachineBasicBlock &MBB,
                                               IndJumpInfoMap &JumpMap) const {
  auto Jumps = llvm::make_filter_range(
      MBB.terminators(), [](auto &MI) { return MI.isIndirectBranch(); });
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto &Linker = ProgCtx.getLinker();
  auto CurrentLoc = Linker.sections().getAddressFor(getMBBSectionName(MBB));
  bool Changed = false;
  for (auto &J : llvm::make_early_inc_range(Jumps)) {
    auto &Info = JumpMap.getInfo(J);
    assert(Info.MBB);
    auto TBBLoc = Linker.sections().getAddressFor(getMBBSectionName(*Info.MBB));
    auto Distance = std::max(TBBLoc, CurrentLoc) - std::min(TBBLoc, CurrentLoc);
    if (Tgt.fitsCondBranch(Distance)) {
      replaceWithBranch(MBB, Info, J, GC, Tgt, JumpMap);
      Changed = true;
    }
  }
  return Changed;
}

bool LongJumpRelaxator::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  auto &JumpMap = getAnalysis<JumpLengthener>().get<IndJumpInfoMap>(MF);
  for (auto &MBB : llvm::drop_begin(MF))
    Changed |= runOnMachineBasicBlock(MBB, JumpMap);
  return Changed;
}

} // namespace snippy
} // namespace llvm
