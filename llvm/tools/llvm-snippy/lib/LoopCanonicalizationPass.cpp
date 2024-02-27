//===-- LoopCanonicalizationPass.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// LoopCanonicalization is preparation pass before LoopLathcer pass. This pass
/// inserts preheaders for loops that don't have them.
///
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Generator/LLVMState.h"
#include "snippy/Support/Options.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

} // namespace snippy
} // namespace llvm

namespace llvm {
namespace snippy {
namespace {

#define DEBUG_TYPE "snippy-loop-canonicalization"
#define PASS_DESC "Snippy Loop Canonicalization"

snippy::opt<bool> ForceLatchTransform(
    "debug-only-make-loop-latch-unconditional",
    cl::desc("Debug/testing only. Split common exiting-latch block into two: "
             "latch and exiting."),
    cl::cat(Options), cl::Hidden);

class LoopCanonicalization final : public MachineFunctionPass {
  void createPreheader(MachineBasicBlock &Header, bool TransferPreds);
  bool insertPreheaderIfNeeded(MachineLoop &ML);
  MachineBasicBlock *splitEdge(MachineBasicBlock &From, MachineBasicBlock &To);
  bool makeLatchUnconditional(MachineLoop &ML, MachineLoopInfo &MLI);
  bool splitExitEdge(MachineLoop &ML);

public:
  static char ID;

  LoopCanonicalization();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char LoopCanonicalization::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LoopCanonicalization;

INITIALIZE_PASS_BEGIN(LoopCanonicalization, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_END(LoopCanonicalization, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createLoopCanonicalizationPass() {
  return new LoopCanonicalization();
}

} // namespace llvm

namespace llvm {
namespace snippy {

LoopCanonicalization::LoopCanonicalization() : MachineFunctionPass(ID) {
  initializeLoopCanonicalizationPass(*PassRegistry::getPassRegistry());
}

bool LoopCanonicalization::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "MachineFunction at the start of "
                       "llvm::snippy::LoopCanonicalization:\n";
             MF.dump());
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &MLI = getAnalysis<MachineLoopInfo>();
  LLVM_DEBUG(dbgs() << "Machine Loop Info for this function:\n");
  LLVM_DEBUG(MLI.getBase().print(dbgs()));
  LLVM_DEBUG(dbgs() << "Consecutive loops:\n");
  LLVM_DEBUG(for_each(GC.getConsecutiveLoops(), [](auto &CLEntry) {
    dbgs() << CLEntry.first << ":";
    for (auto &&CL : CLEntry.second)
      dbgs() << " " << CL;
  }));

  SmallVector<MachineLoop *> Loops;
  copy(MLI, std::back_inserter(Loops));
  bool Changed = false;
  while (!Loops.empty()) {
    auto &ML = *Loops.back();
    Changed |= insertPreheaderIfNeeded(ML);
    if (ForceLatchTransform || GC.hasTrackingMode()) {
      Changed |= splitExitEdge(ML);
      Changed |= makeLatchUnconditional(ML, MLI);
    }
    Loops.pop_back();
    Loops.append(ML.begin(), ML.end());
    ML.verifyLoop();
  }

  return Changed;
}

static void transferPredecessorsExceptLatches(MachineBasicBlock &Preheader,
                                              MachineBasicBlock &Header,
                                              const MachineLoop &ML,
                                              const SnippyTarget &SnippyTgt) {
  assert(ML.contains(&Header));
  assert(!ML.contains(&Preheader));

  SmallVector<MachineBasicBlock *> PredsToTransfer;

  copy_if(Header.predecessors(), std::back_inserter(PredsToTransfer),
          [&ML](auto *Pred) { return !ML.contains(Pred); });

  LLVM_DEBUG(dbgs() << "Header predecessors to transfer: ");
  LLVM_DEBUG(for_each(PredsToTransfer, [](auto *Pred) {
               dbgs() << Pred->getFullName() << ", ";
             }););
  LLVM_DEBUG(dbgs() << "\n");

  for_each(PredsToTransfer, [&SnippyTgt, &Header, &Preheader](auto *Pred) {
    SnippyTgt.replaceBranchDest(*Pred, Header, Preheader);
  });
}

/// If loop header is also a function entry, we need special preheader insertion
void LoopCanonicalization::createPreheader(MachineBasicBlock &Header,
                                           bool TransferPreds) {
  const auto &State =
      getAnalysis<GeneratorContextWrapper>().getContext().getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &MLI = getAnalysis<MachineLoopInfo>();
  const auto *ML = MLI[&Header];
  assert(ML);
  assert(ML->getHeader() == &Header && "Loop header expected");
  auto &MF = *Header.getParent();
  auto *Preheader = MF.CreateMachineBasicBlock();
  assert(Preheader);
  MF.insert(MachineFunction::iterator(&Header), Preheader);
  if (TransferPreds)
    transferPredecessorsExceptLatches(*Preheader, Header, *ML, SnippyTgt);
  SnippyTgt.insertFallbackBranch(*Preheader, Header, State);
  Preheader->addSuccessor(&Header);
  LLVM_DEBUG(Preheader->dump());
}

/// If loop already has preheader, we just get it. If Header has only one
/// predecessor then preheader can be easily inserted if header is entry basic
/// block. In another cases:
///   * Create BB that will be preheader;
///   * Insert pseudo branch to header;
///   * Transfer header's predecessors to preheader except loop's latch;
///   * Set header as preheader successor;
///
/// After all that insert loop init instructions in new created preheader
bool LoopCanonicalization::insertPreheaderIfNeeded(MachineLoop &ML) {
  auto *Header = ML.getHeader();
  assert(Header && "Loop expected to have header");
  assert(Header->pred_size() > 0 &&
         "Loop header should have at least one predecessor");
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto HeaderNum = Header->getNumber();
  if (GC.isNonFirstConsecutiveLoopHeader(HeaderNum)) {
    LLVM_DEBUG(dbgs() << "It's consecutive loop, skip preheader insertion");
    return false;
  }
  if (auto *Preheader = ML.getLoopPreheader()) {
    LLVM_DEBUG(dbgs() << "Loop has preheader, don't split:\n");
    LLVM_DEBUG(Preheader->dump());
    return false;
  }

  LLVM_DEBUG(dbgs() << "Loop doesn't have header, creating preheader:\n");
  auto *MF = Header->getParent();
  bool HeaderIsEntry = Header == &MF->front();
  if (HeaderIsEntry)
    assert(Header->pred_size() == 1 &&
           "Header that is entry must have only one predecessor");
  bool TransferPreds = !HeaderIsEntry;
  createPreheader(*Header, TransferPreds);
  return true;
}

/// This function splits one common exiting and latch block such that latch has
/// only unconditional branch, e.g.:
///
///           -----------
///          /           \
///  +------v-----+      |
///  |            |      |
///  |   header   |      |
///  |            |      |
///  +------+-----+      |
///         |            |
///  +------v--------+   |
///  |               |   |
///  |   exiting /   |   |
///  |   latch       >---/
///  |               |
///  +------+--------+
///         |
///  +------v-----+
///  |            |
///  |    exit    |
///  |            |
///  +------------+
///
///  We change it to
///
///           --------------------------
///          /                          \
///  +------v-----+                     |
///  |            |                     |
///  |   header   |                     |
///  |            |                     |
///  +------+-----+                     |
///         |                           |
///  +------v--------+                  |
///  |               |                  |
///  |    exiting    |                  |
///  |               |                  |
///  +------+-+------+   +-----------+  |
///         | \          |           |  |
///         |  ---------->   latch   |  |
///  +------v-----+      |           |  |
///  |            |      +-----+-----+  |
///  |    exit    |            \        /
///  |            |             --------
///  +------------+
///
bool LoopCanonicalization::makeLatchUnconditional(MachineLoop &ML,
                                                  MachineLoopInfo &MLI) {
  auto *Latch = ML.getLoopLatch();
  assert(Latch && "Expected to have only one latch block.");
  if (Latch->succ_size() == 1)
    // Nothing to do. Latch has only unconditional branch.
    return false;
  assert(Latch->succ_size() == 2 &&
         "Latch block has too many successors. Loops with such a structure are "
         "unsupported.");
  assert(ML.isLoopExiting(Latch) &&
         "Latch with multiple successors must be exiting block as well.");

  auto *Header = ML.getHeader();
  assert(Header && "Loop is expected to have header");

  auto *NewLatch = splitEdge(*Latch, *Header);
  ML.addBasicBlockToLoop(NewLatch, MLI.getBase());
  return true;
}

bool LoopCanonicalization::splitExitEdge(MachineLoop &ML) {
  auto *Exit = ML.getExitBlock();
  auto *Exiting = ML.getExitingBlock();
  assert(Exit && "Expected to have only one exit block.");
  assert(Exiting && "Expected to have only one exiting block.");
  if (Exit->pred_size() == 1)
    return false;

  splitEdge(*Exiting, *Exit);
  return true;
}

MachineBasicBlock *LoopCanonicalization::splitEdge(MachineBasicBlock &From,
                                                   MachineBasicBlock &To) {
  const auto &State =
      getAnalysis<GeneratorContextWrapper>().getContext().getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  assert(From.isSuccessor(&To) && "From -> To is not an edge");

  auto &MF = *From.getParent();
  auto *NewBB = MF.CreateMachineBasicBlock();
  assert(NewBB);
  MF.insert(std::next(MachineFunction::iterator(&From)), NewBB);

  SnippyTgt.insertFallbackBranch(*NewBB, To, State);
  NewBB->addSuccessor(&To);

  SnippyTgt.replaceBranchDest(From, To, *NewBB);
  return NewBB;
}

} // namespace snippy
} // namespace llvm
