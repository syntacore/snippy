//===-- BranchLengthenerPass.cpp --------------------------------*- C++ -*-===//
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
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "snippy-branch-lengthener"
#define PASS_DESC "Snippy Branch Lengthener"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;
static snippy::opt<unsigned> MaxLengthPassCount(
    "max-branch-length-passes", cl::cat(Options), cl::Hidden, cl::init(100),
    cl::desc("maximum number of branch lengthening pass iterations"));

// FIXME: legacy option, remove in next major release.
static snippy::opt<bool> NoRelax("no-branch-relax", cl::cat(Options),
                                 cl::Hidden,
                                 cl::desc("don't relax too far branches"));

static void checkDeprecatedOptionOnce() {
  static bool IsChecked = false;
  if (!NoRelax.isSpecified() || IsChecked)
    return;
  snippy::warn(WarningName::DeprecatedOption, NoRelax.ArgStr,
               " has no effect and will be removed in next major release");
  IsChecked = true;
}

namespace {
class BranchLengthener final : public MachineFunctionPass {
  bool shouldLengthenBranch(MachineInstr &Branch, MachineBasicBlock &MBB,
                            SnippyProgramContext &ProgCtx) const;
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB) const;
  bool runOnce(MachineFunction &MF);

  bool CheckDistance;
  const MachineRegionInfo *MRI = nullptr;
  SmallPtrSet<MachineBasicBlock *, 8> Lengthened;

public:
  static char ID;

  BranchLengthener(bool CheckDistance = false)
      : MachineFunctionPass(ID), CheckDistance(CheckDistance) {}

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
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_END(BranchLengthener, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {
namespace snippy {

StringRef BranchLengthener::getPassName() const { return PASS_DESC " Pass"; }

void BranchLengthener::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  if (CheckDistance)
    AU.addRequired<MachineRegionInfoPass>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

static void lengthenBranch(MachineInstr &Br, MachineBasicBlock &MBB,
                           SnippyProgramContext &ProgCtx) {
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto *BrDest = Tgt.getBranchDestination(Br);
  assert(BrDest);
  auto &State = ProgCtx.getLLVMState();
  Tgt.insertFallbackBranch(MBB, *BrDest, State);
  auto &InsertedJump = MBB.back();

  auto *Symbol = State.getMCContext().createNamedTempSymbol("dest");
  assert(Symbol);
  auto *MFPtr = MBB.getParent();
  assert(MFPtr);
  auto &MF = *MFPtr;
  InsertedJump.setPreInstrSymbol(MF, Symbol);
  Tgt.replaceBranchDest(Br, InsertedJump);
}

bool BranchLengthener::shouldLengthenBranch(
    MachineInstr &Branch, MachineBasicBlock &MBB,
    SnippyProgramContext &ProgCtx) const {
  assert(MRI);
  auto *PR = MRI->getRegionFor(&MBB);
  assert(PR);
  auto &R = *PR;

  assert(Branch.isBranch() && "Only branches expected");
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

  [[maybe_unused]] auto MaxInstrSize = SnippyTgt.getMaxInstrSize();
  auto MaxBranchDstMod = SnippyTgt.getMaxBranchDstMod(Branch.getOpcode());
  if (DistanceInBytes < MaxBranchDstMod)
    return false;
  // Print '0x' + 4 significant digits
  [[maybe_unused]] constexpr auto HexPrintWidth = 6;

  LLVM_DEBUG(dbgs() << "Far branch to lengthen: " << Branch << '\n');
  LLVM_DEBUG(dbgs() << "In block: " << MBB.getFullName() << '\n');
  LLVM_DEBUG(dbgs() << "With distance: "
                    << format_hex(DistanceInBytes, HexPrintWidth) << '\n');
  LLVM_DEBUG(dbgs() << ">=\n");
  LLVM_DEBUG(dbgs() << "Max distance:  "
                    << format_hex(MaxBranchDstMod, HexPrintWidth) << '\n');
  return true;
}

bool BranchLengthener::runOnMachineBasicBlock(MachineBasicBlock &MBB) const {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = GC.getProgramContext();
  auto FirstTermIt = MBB.getFirstTerminator();
  if (FirstTermIt == MBB.end())
    return false;
  auto &FirstTerm = *FirstTermIt;
  if (!FirstTerm.isConditionalBranch())
    return false;
  if (CheckDistance && !shouldLengthenBranch(FirstTerm, MBB, ProgCtx))
    return false;
  lengthenBranch(FirstTerm, MBB, ProgCtx);

  return true;
}

bool BranchLengthener::runOnce(MachineFunction &MF) {
  bool Changed = false;
  for (auto &MBB : MF) {
    if (CheckDistance && Lengthened.count(&MBB))
      continue;
    if (runOnMachineBasicBlock(MBB)) {
      Changed = true;
      if (CheckDistance)
        Lengthened.insert(&MBB);
    }
  }
  return Changed;
}

bool BranchLengthener::runOnMachineFunction(MachineFunction &MF) {
  checkDeprecatedOptionOnce();
  if (!CheckDistance)
    return runOnce(MF);
  MRI = &getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  Lengthened.clear();

  bool Changed = false;
  const unsigned MaxLenPasses = MaxLengthPassCount;
  unsigned I = 0;
  // Changes may increase code size and branch distances, therefore keep
  // lengthening until code is stable.
  for (; I < MaxLenPasses && runOnce(MF); ++I)
    Changed = true;

  if (I >= MaxLenPasses)
    snippy::fatal(
        PASS_DESC,
        llvm::formatv("branch distances unstable after {} relaxation passes. "
                      "You can increase iteration limit using '{}' option",
                      MaxLenPasses, MaxLengthPassCount.ArgStr));
  MRI = nullptr;
  return Changed;
}

} // namespace snippy

MachineFunctionPass *createBranchLengthenerPass(bool CheckDistance) {
  return new snippy::BranchLengthener(CheckDistance);
}

} // namespace llvm
