//===-- BranchRelaxatorPass.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Support/Options.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "snippy-branch-relaxator"
#define PASS_DESC "Snippy Branch Relaxator"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

snippy::opt<bool> NoRelax("no-branch-relax", cl::cat(Options),
                          cl::desc("don't relax too far branches"));

namespace {

class BranchRelaxator final : public MachineFunctionPass {
public:
  static char ID;

  BranchRelaxator() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineBasicBlock(MachineBasicBlock &MBB);

  bool runOnMachineFunction(MachineFunction &MF) override;

  bool tryRelaxBranch(MachineInstr &Branch, const MachineRegion &R) const;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineRegionInfoPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char BranchRelaxator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::BranchRelaxator;

INITIALIZE_PASS_BEGIN(BranchRelaxator, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_END(BranchRelaxator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createBranchRelaxatorPass() {
  return new BranchRelaxator();
}

namespace snippy {
bool BranchRelaxator::tryRelaxBranch(MachineInstr &Branch,
                                     const MachineRegion &R) const {
  assert(Branch.isBranch() && "Only branches expected");
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  const auto *DstMBB = SnippyTgt.getBranchDestination(Branch);
  assert(DstMBB);
  const auto *BranchMBB = Branch.getParent();
  assert(BranchMBB);
  const auto *Entry = R.getEntry();
  const auto *Exit = R.getExit();
  const auto *FwdBrStart = Entry;
  const auto *BwdBrStart = Exit ? Exit->getPrevNode() : nullptr;
  bool ForwardBranch = BranchMBB == FwdBrStart;
  bool BackwardBranch = BranchMBB == BwdBrStart;
  if (!((ForwardBranch && (DstMBB == Exit)) ||
        (BackwardBranch && (DstMBB == FwdBrStart)))) {
    assert(DstMBB == BranchMBB->getNextNode() &&
           "Branch destination expected to be fallback");
    return false;
  }

  unsigned DistanceInBytes = 0;

  if (BackwardBranch) {
    // From first instruction to branch inclusively
    DistanceInBytes += State.getCodeBlockSize(
        BranchMBB->begin(),
        std::next(MachineBasicBlock::const_iterator(Branch)));
    // if loop consists of only one block then we don't want to count it twice
    if (BranchMBB != DstMBB)
      DistanceInBytes += State.getCodeBlockSize(DstMBB->begin(), DstMBB->end());
  } else {
    DistanceInBytes += State.getCodeBlockSize(
        MachineBasicBlock::const_iterator(Branch), BranchMBB->end());
  }

  // we already counted corner cases, now just count everything between
  DistanceInBytes = std::accumulate(
      R.block_begin(), R.block_end(), DistanceInBytes,
      [&](unsigned Dist, auto *BB) {
        assert(BB);
        bool DontTakeBB = (BB == Entry) || (BB == Exit) || (BB == BranchMBB);
        unsigned BBSize =
            DontTakeBB ? 0 : State.getCodeBlockSize(BB->begin(), BB->end());
        return Dist + BBSize;
      });

  auto MaxInstrSize = SnippyTgt.getMaxInstrSize();
  auto MaxBranchDstMod = SnippyTgt.getMaxBranchDstMod(Branch.getOpcode());
  if (DistanceInBytes < MaxBranchDstMod)
    return false;

  // Print '0x' + 4 significant digits
  [[maybe_unused]] constexpr auto HexPrintWidth = 6;
  LLVM_DEBUG(dbgs() << "Trying to relax (dist == "
                    << format_hex(DistanceInBytes, HexPrintWidth) << ") in "
                    << BranchMBB->getFullName() << ": ";
             Branch.dump());

  if (NoRelax || !SnippyTgt.relaxBranch(Branch, DistanceInBytes, ProgCtx)) {
    LLVM_DEBUG(Branch.dump());
    LLVM_DEBUG(R.dump());
    LLVM_DEBUG(dbgs() << "Distance = " << DistanceInBytes << '\n');
    LLVM_DEBUG(dbgs() << "MaxInstrSize = " << MaxInstrSize << '\n');
    LLVM_DEBUG(dbgs() << "MaxBranchDstMod = " << MaxBranchDstMod << '\n');
    fatal(State.getCtx(), "Generation error", "Too far branch was generated");
  }
  return true;
}

bool BranchRelaxator::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  auto &State = getAnalysis<GeneratorContextWrapper>()
                    .getContext()
                    .getProgramContext()
                    .getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();

  auto FirstTermIter = MBB.getFirstInstrTerminator();
  if (FirstTermIter == MBB.end())
    return false;

  // FIXME: it works for RISCV, not sure it's acceptable for other platforms
  auto &FirstTerm = *FirstTermIter;
  if (!FirstTerm.isBranch())
    return false;

  if (!SnippyTgt.branchNeedsVerification(FirstTerm))
    return false;

  auto *R = MRI.getRegionFor(&MBB);
  assert(R);
  return tryRelaxBranch(FirstTerm, *R);
}

bool BranchRelaxator::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  // Hope changes in one MBB don't affect any other
  for (auto &MBB : MF)
    Changed |= runOnMachineBasicBlock(MBB);

  return Changed;
}

} // namespace snippy
} // namespace llvm
