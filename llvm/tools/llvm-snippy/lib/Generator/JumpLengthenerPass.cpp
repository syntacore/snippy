//===-- JumpLengthenerPass.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/JumpLengthenerPass.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Policy.h"

#include "../InitializePasses.h"
#include "snippy/Generator/SMCManager.h"

#define DEBUG_TYPE "snippy-jump-lengthener"
#define PASS_DESC "Snippy Jump Lengthener"

namespace llvm {
namespace snippy {
char JumpLengthener::ID = 0;
} // namespace snippy

snippy::ActiveImmutablePassInterface *createJumpLengthenerPass() {
  return new snippy::JumpLengthener;
}

} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::JumpLengthener;

SNIPPY_INITIALIZE_PASS(JumpLengthener, DEBUG_TYPE, PASS_DESC, false)

namespace llvm {
namespace snippy {

static void lengthenJump(MachineInstr &J, InstructionGenerationContext &IGC,
                         const SnippyTarget &Tgt, IndJumpInfoMap &JumpMap) {
  auto *MBBPtr = J.getParent();
  assert(MBBPtr);
  auto &MBB = *MBBPtr;
  auto *TBB = Tgt.getBranchDestination(J);
  assert(TBB);
  auto &ProgCtx = IGC.ProgCtx;

  auto &GP = ProgCtx.getOrAddGlobalsPoolFor(
      IGC.getSnippyModule(), "Failed to allocate space for relocation for BB "
                             "address (jump lengthener)");
  auto *GV = getGVForMBB(*TBB, GP, ProgCtx);
  auto Addr = GP.getGVAddress(GV);
  auto FirstTerm = MBB.getFirstInstrTerminator();
  // Point right before first terminator
  auto Prev = std::make_reverse_iterator(FirstTerm);
  IGC.Ins = std::next(J.getIterator());

  auto NewJump = Tgt.insertJumpThroughRelocation(IGC, Addr);
  NewJump->cloneInstrSymbols(*MBB.getParent(), J);
  // Instruction right after Prev instr
  auto FirstInserted = Prev.base();
  auto InstrPtrs = llvm::map_range(llvm::make_range(FirstInserted, FirstTerm),
                                   [](auto &MI) { return &MI; });

  JumpMap.addJump(*NewJump,
                  IndJumpInfo{TBB, {InstrPtrs.begin(), InstrPtrs.end()}});
}

StringRef JumpLengthener::getPassName() const { return PASS_DESC " Pass"; }

void JumpLengthener::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool JumpLengthener::runOnMachineBasicBlock(MachineBasicBlock &MBB,
                                            IndJumpInfoMap &JumpMap) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto FirstTermIt = MBB.getFirstTerminator();
  if (FirstTermIt == MBB.end())
    return false;

  auto Jumps = llvm::make_filter_range(
      MBB.terminators(), [](auto &MI) { return MI.isUnconditionalBranch(); });
  InstructionGenerationContext IGC{MBB, MBB.getFirstTerminator(), GC};
  auto RP = IGC.pushRegPool();

  for (auto &J : llvm::make_early_inc_range(Jumps)) {
    lengthenJump(J, IGC, Tgt, JumpMap);
    J.eraseFromParent();
  }
  return true;
}

bool JumpLengthener::runOnMachineFunction(MachineFunction &MF) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  if (!GC.getConfig().PassCfg.InstrsGenerationConfig.NeedsRelocations)
    return false;
  bool Changed = false;
  auto &JumpMap = get<IndJumpInfoMap>(MF);
  for (auto &MBB : MF)
    Changed |= runOnMachineBasicBlock(MBB, JumpMap);
  return Changed;
}

} // namespace snippy
} // namespace llvm
