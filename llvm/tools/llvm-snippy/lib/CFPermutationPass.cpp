//===-- CFPermutationPass.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Control Flow Permutation pass implementation. This pass takes machine
/// function with only blocks and fallback branches and permutes them saving
/// structured control flow (CF).
///
/// Algorithm in general:
///   1. Take random BB for permutation (branch source);
///   2. Take random BB from available set for selected BB (branch destination);
///   3. Update available sets and other service info;
///   4. Return to 1. until all branches are permuted.
///
/// Available set is a support data structure that helps to track candidates for
/// branch destination and save structured CF.
///
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>
#include <limits>
#include <type_traits>

#define DEBUG_TYPE "snippy-cf-permutation"
#define PASS_DESC "Snippy Control Flow Permutation"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

} // namespace snippy
} // namespace llvm

namespace llvm {
namespace snippy {
namespace {

snippy::opt<bool>
    PermutationProgress("permutation-progress",
                        cl::desc("Show branch permutation progress."),
                        cl::cat(Options), cl::init(false), cl::Hidden);

snippy::opt<bool> PermutationStatus("permutation-status",
                                    cl::desc("Show branch permutation status."),
                                    cl::cat(Options), cl::init(false),
                                    cl::Hidden);

class CFPermutation final : public MachineFunctionPass {
public:
  struct BlockInfo final {
    unsigned Successor;
    unsigned IfDepth = 0;
    unsigned LoopDepth = 0;
    using SetT = std::set<unsigned>;
    SetT Available;

    BlockInfo(unsigned Succ, const SetT &Available)
        : Successor(Succ), Available(Available) {}
  };

  using BlocksInfoT = SmallVector<BlockInfo>;
  using BlocksInfoIter = BlocksInfoT::iterator;

private:
  /// BlocksInfo is a table with permutation context infromation.
  /// It has next structure and initial value (for N blocks or N - 1 branches):
  ///
  /// | BB Idx | Successor | IfDepth | LoopDepth | AvailableSet  |
  /// +--------+-----------+---------+-----------+---------------+
  /// |      0 |         1 |       0 |         0 | { 0, ..., N } |
  /// |      1 |         2 |       0 |         0 | { 0, ..., N } |
  /// |    ... |       ... |       0 |         0 | { 0, ..., N } |
  /// |      k |     k + 1 |       0 |         0 | { 0, ..., N } |
  /// |    ... |       ... |       0 |         0 | { 0, ..., N } |
  /// |  N - 1 |         N |       0 |         0 | { 0, ..., N } |
  BlocksInfoT BlocksInfo;
  MachineFunction *CurrMF = nullptr;
  const Branchegram *BranchSettings = nullptr;
  unsigned PermutationCounter = 0;

public:
  static char ID;

  CFPermutation();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  void releaseMemory() override { BlocksInfo.clear(); }

  void print(raw_ostream &OS, const Module *M) const override;
  void dump() const;

private:
  void initBlocksInfo(unsigned Size);
  unsigned calculateMaxDistance(unsigned BBNum, unsigned Size);
  void makePermutation(ArrayRef<unsigned> PermutationOrder);
  void permuteBlock(unsigned BB);
  void updateBlocksInfo(unsigned From, unsigned To);
  void dumpPermutedBranch(unsigned BB, bool IsLoop, const BlockInfo &BI) const;
  bool updateBranches(MachineFunction &MF);

  BlocksInfoIter findMaxIfDepthReached(BlocksInfoIter Beg, BlocksInfoIter End);
  BlocksInfoIter findMaxLoopDepthReached(BlocksInfoIter Beg,
                                         BlocksInfoIter End);
};

char CFPermutation::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CFPermutation;

INITIALIZE_PASS_BEGIN(CFPermutation, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(CFPermutation, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createCFPermutationPass() { return new CFPermutation(); }

namespace snippy {

CFPermutation::CFPermutation() : MachineFunctionPass(ID) {
  initializeCFPermutationPass(*PassRegistry::getPassRegistry());
}

/// If max distance isn't specified then this function is used to calculate max
/// distance for a branch in the selected basic block.
unsigned CFPermutation::calculateMaxDistance(unsigned BBNum, unsigned Size) {
  LLVM_DEBUG(dbgs() << "Calculating max distance for BB#" << BBNum << '\n');
  if (BranchSettings->NConsecutiveLoops != 0)
    return 0;
  assert(CurrMF);
  auto *BB = CurrMF->getBlockNumbered(BBNum);
  assert(BB);
  auto Branch = BB->getFirstTerminator();
  assert(Branch != BB->end());
  const auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &SnippyTgt = GC.getLLVMState().getSnippyTarget();
  auto PCDist = BranchSettings->getPCDistance();
  auto MaxBranchDstMod =
      PCDist.Max.value_or(SnippyTgt.getMaxBranchDstMod(Branch->getOpcode()));
  auto MaxInstrSize = SnippyTgt.getMaxInstrSize();
  if (MaxBranchDstMod < MaxInstrSize)
    snippy::fatal(GC.getLLVMState().getCtx(),
                  "Max PC distance (" + Twine(MaxBranchDstMod) +
                      ") is less than max instruction size",
                  Twine(MaxInstrSize));

  double AverageBBNInstrs =
      GC.getRequestedInstrsNum(*CurrMF) / static_cast<double>(Size);
  // NOTE: we can have other overhead, for example, load/store. May be it worth
  // to consider them too.
  unsigned LoopOverhead = SnippyTgt.getLoopOverhead();
  // NLoops = LoopRatio * Size
  // AverageLoopOverheadPerBB = LoopOverhead * NLoops / Size;
  // AverageLoopOverheadPerBB = LoopOverhead * LoopRatio * Size / Size;
  double AverageLoopOverheadPerBB = LoopOverhead * BranchSettings->LoopRatio;
  AverageBBNInstrs += AverageLoopOverheadPerBB;
  assert(std::isfinite(AverageBBNInstrs) &&
         "Average number of instructions per BB must be finite");

  // NOTE: may be it is better to calculate weighted sum in future
  double MaxDistanceForThisBranch =
      MaxBranchDstMod / (AverageBBNInstrs * MaxInstrSize) - 1;
  assert(MaxDistanceForThisBranch <= std::numeric_limits<unsigned>::max() &&
         "Max distance doesn't fit in unsigned");
  return MaxDistanceForThisBranch;
}

[[maybe_unused]] static void
printRangesForAvailableSet(raw_ostream &OS, bool BackwardPossible,
                           unsigned BackwardSeqBeg, unsigned BackwardSeqEnd,
                           bool ForwardPossible, unsigned ForwardSeqBeg,
                           unsigned ForwardSeqEnd) {
  auto PrintRange = [&OS](bool NotEmpty, unsigned Begin, unsigned End) {
    if (NotEmpty)
      OS << Begin << ':' << End;
    else
      OS << "empty (" << Begin << ':' << End << ")";
  };
  OS << "Making available set from range(s):\n";
  OS << "Backward: ";
  PrintRange(BackwardPossible, BackwardSeqBeg, BackwardSeqEnd);
  OS << '\n';
  OS << "Forward: ";
  PrintRange(ForwardPossible, ForwardSeqBeg, ForwardSeqEnd);
  OS << '\n';
}

[[maybe_unused]] void
printAvailableSet(raw_ostream &OS, unsigned BB,
                  const CFPermutation::BlockInfo::SetT &Available) {
  OS << "Initializing BlocksInfo for BB#" << BB << " with set: {";
  for (auto &&Candidtate : Available)
    OS << Candidtate << ',';
  OS << "}\n";
}

void CFPermutation::initBlocksInfo(unsigned Size) {
  BlocksInfo.reserve(Size);
  if (BranchSettings->NConsecutiveLoops > 0) {
    assert(BranchSettings->LoopRatio == 1.0 &&
           BranchSettings->getMaxLoopDepth() == 1 &&
           BranchSettings->getBlockDistance().Min.value_or(0) == 0 &&
           BranchSettings->getBlockDistance().Max.value_or(0) == 0 &&
           "unsupported branch settings");
    transform(seq(0u, Size), std::back_inserter(BlocksInfo), [](unsigned BB) {
      return BlockInfo(BB + 1, BlockInfo::SetT({BB, BB + 1}));
    });
    return;
  }

  bool AutoMaxBBDistance = !BranchSettings->getBlockDistance().Max.has_value();
  LLVM_DEBUG(dbgs() << "Initializing BlocksInfo for " << Size << " blocks\n");
  LLVM_DEBUG(dbgs() << (AutoMaxBBDistance ? "" : "don't ")
                    << "calculate max distance adaptively\n");
  for (auto BB : seq(0u, Size)) {
    assert(BlocksInfo.size() == BB);
    // We don't want want to permute unconditional branches
    if (CurrMF->getBlockNumbered(BB)
            ->getFirstTerminator()
            ->isUnconditionalBranch()) {
      BlocksInfo.emplace_back(BB + 1, BlockInfo::SetT());
      continue;
    }

    unsigned MinBBDst = BranchSettings->getBlockDistance().Min.value_or(0);
    unsigned MaxBBDst = AutoMaxBBDistance
                            ? calculateMaxDistance(BB, Size)
                            : *BranchSettings->getBlockDistance().Max;
    if (MaxBBDst > static_cast<unsigned>(std::numeric_limits<int>::max()))
      report_fatal_error(
          "Max block distance doesn't fit int: " + Twine(MaxBBDst), false);
    // Loop branch has two more blocks with instructions between source and
    // destination PC comparing with if branch (it is first and last blocks).
    // Thus we need to decrease max distance of backward branch.
    int BackwardDst =
        std::min<int>(BB + 1, BB - MaxBBDst + (AutoMaxBBDistance ? 2 : 0));
    unsigned BegLimit = std::max<int>(0, BackwardDst);
    unsigned EndLimit = BB + MaxBBDst;

    // We don't generate loops
    if (BranchSettings->LoopRatio == 0 ||
        (BranchSettings->getMaxLoopDepth() == 0))
      BegLimit = BB + 1;

    // We generate only loops
    if (BranchSettings->LoopRatio == 1 ||
        (BranchSettings->hasMaxIfDepth() &&
         BranchSettings->getMaxIfDepth() == 0))
      EndLimit = BB;

    // Setting initial available set for BB as
    // [BB - MaxDst, BB - MinDst] U (BB + MinDst, BB + MaxDst]
    // to preserve min and max distance
    unsigned BackwardSeqBeg = BegLimit;
    unsigned BackwardSeqEnd = BB < MinBBDst ? 0 : BB - MinBBDst;
    bool BackwardPossible = BB >= MinBBDst && BackwardSeqBeg <= BackwardSeqEnd;
    unsigned ForwardSeqBeg = BB + std::max(MinBBDst, 1u);
    unsigned ForwardSeqEnd = std::min(Size, EndLimit);
    bool ForwardPossible = ForwardSeqBeg <= ForwardSeqEnd;

    LLVM_DEBUG(printRangesForAvailableSet(
        dbgs(), BackwardPossible, BackwardSeqBeg, BackwardSeqEnd,
        ForwardPossible, ForwardSeqBeg, ForwardSeqEnd));

    BlockInfo::SetT Available;
    if (BackwardPossible) {
      auto BackwardSeq = seq_inclusive(BackwardSeqBeg, BackwardSeqEnd);
      Available.insert(BackwardSeq.begin(), BackwardSeq.end());
    }
    if (ForwardPossible) {
      auto ForwardSeq = seq_inclusive(ForwardSeqBeg, ForwardSeqEnd);
      Available.insert(ForwardSeq.begin(), ForwardSeq.end());
    }

    LLVM_DEBUG(printAvailableSet(dbgs(), BB, Available));

    // Constructing initial BlockInfo for BB:
    //   Successor: BB + 1
    //   Initial available set: Available
    BlocksInfo.emplace_back(BB + 1, std::move(Available));
  }
}

static auto generatePermutationOrder(unsigned Size) {
  auto Seq = seq(0u, Size);
  SmallVector<unsigned> Order(Seq.begin(), Seq.end());
  std::shuffle(Order.begin(), Order.end(), RandEngine::engine());

  LLVM_DEBUG(dbgs() << "Permutation order:\n");
  LLVM_DEBUG(dbgs() << Order.front());
  LLVM_DEBUG(
      for_each(drop_begin(Order), [](auto BB) { dbgs() << ", " << BB; }));
  LLVM_DEBUG(dbgs() << '\n');

  return Order;
}

void CFPermutation::makePermutation(ArrayRef<unsigned> PermutationOrder) {
  LLVM_DEBUG(dbgs() << "Available sets before permutation:\n");
  LLVM_DEBUG(dump());

  for_each(PermutationOrder, [this](auto BB) {
    if (PermutationProgress) {
      errs() << "\rPermuting " << ++PermutationCounter << '/'
             << BlocksInfo.size();
    }

    permuteBlock(BB);
  });

  if (PermutationProgress) {
    errs() << '\n';
  }
}

bool CFPermutation::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(
      dbgs() << "CFPermutation::runOnMachineFunction runs on function:\n");
  LLVM_DEBUG(MF.dump());
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  if (GC.hasTrackingMode() && GC.isLoopGenerationPossible() &&
      !GC.getOrCreateSimRunner()
           .getPrimaryInterpreter()
           .modelSupportCallbacks())
    fatal(GC.getLLVMState().getCtx(),
          "Loops cannot be generated in selfcheck/backtrack/hazard modes",
          "Selected model does not support it");
  BranchSettings = &GC.getGenSettings().Cfg.Branches;
  LLVM_DEBUG(BranchSettings->dump());
  PermutationCounter = 0;
  auto CFInstrsNum = GC.getCFInstrsNum(MF);
  if (CFInstrsNum == 0)
    return false;

  assert(MF.size() - 1 == CFInstrsNum && "MF expected to have all blocks");
  CurrMF = &MF;
  initBlocksInfo(CFInstrsNum);
  auto PermutationOrder = generatePermutationOrder(CFInstrsNum);
  makePermutation(PermutationOrder);
  return updateBranches(MF);
}

void CFPermutation::dumpPermutedBranch(unsigned BB, bool IsLoop,
                                       const BlockInfo &BI) const {
  auto Selected = BI.Successor;
  if (PermutationStatus) {
    if (PermutationProgress)
      errs() << '\n';
    errs() << "Making branch from " << BB << " to " << Selected << ", "
           << (IsLoop ? "loop" : "if") << ", distance: "
           << std::abs(static_cast<int>(BB) - static_cast<int>(Selected) +
                       (IsLoop ? 1 : -1))
           << ", depth: " << (IsLoop ? BI.LoopDepth : BI.IfDepth) + 1 << "\n";
#if defined(LLVM_ENABLE_DUMP)
    CurrMF->getBlockNumbered(BB)->getFirstTerminator()->dump();
#endif
  } else {
    LLVM_DEBUG(dbgs() << "Making branch from " << BB << " to " << Selected
                      << "\n");
    LLVM_DEBUG(CurrMF->getBlockNumbered(BB)->getFirstTerminator()->dump());
  }
}

static auto calcCandidatesDistribution(unsigned BackwardCount,
                                       unsigned ForwardCount,
                                       double LoopRatio) {
  if (BackwardCount == 0 || ForwardCount == 0)
    return std::discrete_distribution<unsigned>(
        BackwardCount + ForwardCount, 1.0, 1.0, [](auto W) { return W; });

  double BackwardWeight = ForwardCount / BackwardCount * LoopRatio;
  double ForwardWeight = BackwardCount / ForwardCount * (1 - LoopRatio);

  unsigned Counter = 0;
  auto DD = std::discrete_distribution<unsigned>(
      BackwardCount + ForwardCount, BackwardWeight, ForwardWeight,
      [=](auto Weight) mutable {
        return ++Counter <= BackwardCount ? BackwardWeight : ForwardWeight;
      });

  LLVM_DEBUG(dbgs() << "DD for selecting blocks:");
  LLVM_DEBUG(
      for_each(DD.probabilities(), [](double W) { dbgs() << " " << W; }));
  LLVM_DEBUG(dbgs() << "\n");

  return DD;
}

static unsigned selectBB(const CFPermutation::BlockInfo::SetT &Available,
                         unsigned CurrBB, double LoopRatio) {
  auto BackwardCount =
      count_if(Available, [CurrBB](auto Other) { return Other <= CurrBB; });
  auto ForwardCount = Available.size() - BackwardCount;

  auto DD = calcCandidatesDistribution(BackwardCount, ForwardCount, LoopRatio);
  return RandEngine::selectFromContainer(Available, DD);
}

void CFPermutation::permuteBlock(unsigned BB) {
  assert(BB < BlocksInfo.size() && "BB out of range");
  LLVM_DEBUG(dbgs() << "Permuting BB#" << BB << '\n');

  auto BI = std::next(BlocksInfo.begin(), BB);
  if (BI->Available.empty()) {
    LLVM_DEBUG(dbgs() << "Don't permute BB#" << BB
                      << " because available set is empty\n");
    return;
  }

  unsigned NConsecutiveLoops = BranchSettings->NConsecutiveLoops;
  // We cannot always create N consecutive loops
  auto NextCantBeSingleBlockLoopPos =
      std::find_if(BI, BI + NConsecutiveLoops + 1, [this](const auto &NextBI) {
        if (NextBI.Available.empty())
          return true;
        return NextBI.Available.count(&NextBI - BlocksInfo.data()) == 0;
      });
  NConsecutiveLoops = (NextCantBeSingleBlockLoopPos > BI)
                          ? (NextCantBeSingleBlockLoopPos - BI - 1)
                          : 0;
  unsigned Selected =
      (NConsecutiveLoops == 0)
          ? selectBB(BI->Available, BB, BranchSettings->LoopRatio)
          : BB;
  bool IsLoop = BB >= Selected;
  BI->Successor = Selected;
  // Clear available set since we already choosed destination and don't need to
  // track requirements for this BB
  BI->Available.clear();

  dumpPermutedBranch(BB, IsLoop, *BI);
  updateBlocksInfo(BB, Selected);
  LLVM_DEBUG(dbgs() << "Available sets after this permutation:\n");
  LLVM_DEBUG(dump());

  if (IsLoop && NConsecutiveLoops > 0) {
    auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
    for (auto NextBB : seq_inclusive(BB + 1, BB + NConsecutiveLoops)) {
      LLVM_DEBUG(dbgs() << "Permuting BB#" << NextBB
                        << " (consecutive loop)\n");
      GC.registerConsecutiveLoopsHeader(NextBB, BB);
      auto &NextBI = BlocksInfo[NextBB];
      assert(NextBI.Available.count(NextBB));
      NextBI.Successor = NextBB;
      NextBI.Available.clear();
      dumpPermutedBranch(NextBB, /* IsLoop */ true, NextBI);
      updateBlocksInfo(NextBB, NextBB);
      LLVM_DEBUG(
          dbgs() << "Available sets after consecutive loop permutation:\n");
      LLVM_DEBUG(dump());
    }
  }
}

// Simple function for finding if max depth reached
static typename CFPermutation::BlocksInfoIter
findMaxDepthReachedImpl(typename CFPermutation::BlocksInfoIter Beg,
                        typename CFPermutation::BlocksInfoIter End,
                        typename CFPermutation::BlocksInfoT &BlocksInfo,
                        unsigned CFPermutation::BlockInfo::*DepthToCheck,
                        unsigned DepthLimit) {
  assert(Beg <= End && "Respect iteration order");
  return std::find_if(Beg, End, [=, &BlocksInfo](const auto &BI) {
    assert(BI.*DepthToCheck <= DepthLimit &&
           "Depth expected to be not more than MaxDepth");
    bool Reached = BI.*DepthToCheck == DepthLimit;
    if (Reached)
      LLVM_DEBUG(dbgs() << "MaxDepth reached for BB#"
                        << &BI - BlocksInfo.begin() << '\n');
    return Reached;
  });
}

CFPermutation::BlocksInfoIter
CFPermutation::findMaxIfDepthReached(BlocksInfoIter Beg, BlocksInfoIter End) {
  if (!BranchSettings->hasMaxIfDepth())
    return End;
  return findMaxDepthReachedImpl(Beg, End, BlocksInfo, &BlockInfo::IfDepth,
                                 BranchSettings->getMaxIfDepth());
}

CFPermutation::BlocksInfoIter
CFPermutation::findMaxLoopDepthReached(BlocksInfoIter Beg, BlocksInfoIter End) {
  return findMaxDepthReachedImpl(Beg, End, BlocksInfo, &BlockInfo::LoopDepth,
                                 BranchSettings->getMaxLoopDepth());
}

static void eraseRangeFromSet(
    typename CFPermutation::BlockInfo::SetT &Set,
    typename CFPermutation::BlockInfo::SetT::value_type LowerBound,
    typename CFPermutation::BlockInfo::SetT::value_type UpperBound) {
  auto Beg = Set.lower_bound(LowerBound);
  auto End = Set.upper_bound(UpperBound);
  Set.erase(Beg, End);
}

static void preserveDepth(typename CFPermutation::BlocksInfoIter Pos,
                          typename CFPermutation::BlocksInfoIter Beg,
                          typename CFPermutation::BlocksInfoIter End,
                          unsigned LowerBound, unsigned UpperBound) {
  std::for_each(Beg, End, [LowerBound, UpperBound](auto &BI) {
    eraseRangeFromSet(BI.Available, LowerBound, UpperBound);
  });
  Pos->Available.clear();
}

// In this function region is a created region by branch From->To
void CFPermutation::updateBlocksInfo(unsigned From, unsigned To) {
  LLVM_DEBUG(dbgs() << "Update blocks info for " << From << ":" << To << "\n");
  unsigned Size = BlocksInfo.size();
  assert(From < Size && "Branch source is out of range");
  assert(To <= Size && "Branch destination is out of range");

  bool IsLoop = From >= To;
  unsigned Entry = IsLoop ? To : From;
  unsigned Exit = IsLoop ? From : To;

  // For each block inside newly created region erase it from available sets of
  // blocks outside this region. Also branches from inside region to its entry
  // are restricted.
  auto ProcessBlocksInsideRegion = [Entry, Exit, Size, IsLoop](auto &BI) {
    eraseRangeFromSet(BI.Available, 0, Entry);
    eraseRangeFromSet(BI.Available, Exit, Size);
    if (IsLoop)
      ++BI.LoopDepth;
    else
      ++BI.IfDepth;
  };

  // For each block outside newly created region erase it from available sets of
  // blocks inside this region.
  auto ProcessBlocksOutsideRegion = [Entry, Exit](auto &BI) {
    eraseRangeFromSet(BI.Available, Entry, Exit);
  };

  auto Beg = BlocksInfo.begin();
  auto End = BlocksInfo.end();
  // Different ranges for if and loop because loop goes from exit end and if
  // goes up to exit start.
  auto RegionUpperBound = Beg + Entry + (IsLoop ? 0 : 1);
  auto RegionLowerBound = Beg + Exit + (IsLoop ? 1 : 0);
  assert(RegionUpperBound <= End);
  assert(RegionLowerBound <= End);
  // Accurately process all blocks after permutation
  std::for_each(Beg, RegionUpperBound, ProcessBlocksOutsideRegion);
  std::for_each(RegionUpperBound, RegionLowerBound, ProcessBlocksInsideRegion);
  std::for_each(RegionLowerBound, End, ProcessBlocksOutsideRegion);
  LLVM_DEBUG(dump());

  // If depth control: disable branches with source before and destination after
  // BB that reached max depth
  for (auto Pos = findMaxIfDepthReached(RegionUpperBound, RegionLowerBound);
       Pos != RegionLowerBound;
       Pos = findMaxIfDepthReached(std::next(Pos), RegionLowerBound)) {
    auto PosNum = Pos - Beg;
    preserveDepth(Pos, Beg, Pos, PosNum, Size);
  }

  // Loop depth control: disable branches with destination before and source
  // after BB that reached max depth
  for (auto Pos = findMaxLoopDepthReached(RegionUpperBound, RegionLowerBound);
       Pos != RegionLowerBound;
       Pos = findMaxLoopDepthReached(std::next(Pos), RegionLowerBound)) {
    auto PosNum = Pos - Beg;
    preserveDepth(Pos, Pos, End, 0, PosNum);
  }
}

// Apply calculated CF
bool CFPermutation::updateBranches(MachineFunction &MF) {
  const auto &SnippyTgt = getAnalysis<GeneratorContextWrapper>()
                              .getContext()
                              .getLLVMState()
                              .getSnippyTarget();
  bool Changed = false;
  for (auto BBNum : seq<unsigned>(0, BlocksInfo.size())) {
    auto *BB = MF.getBlockNumbered(BBNum);
    assert(BB);
    auto BranchPos = BB->getFirstTerminator();
    assert(BranchPos != BB->end());
    auto &Branch = *BranchPos;
    auto SuccNum = BlocksInfo[BBNum].Successor;
    assert((SuccNum > BBNum || Branch.isConditionalBranch()) &&
           "Loop are not supported for contidional branches");
    auto *NewDest = MF.getBlockNumbered(SuccNum);
    assert(NewDest);
    Changed |= SnippyTgt.replaceBranchDest(Branch, *NewDest);
  }

  return Changed;
}
void CFPermutation::print(raw_ostream &OS, const Module *M) const {
  for (auto BB : seq<unsigned>(0, BlocksInfo.size())) {
    OS << BB << " : " << BlocksInfo[BB].Successor << " : "
       << BlocksInfo[BB].IfDepth << " : " << BlocksInfo[BB].LoopDepth << " : {";
    for_each(BlocksInfo[BB].Available,
             [&OS](auto Available) { OS << ' ' << Available << ","; });
    OS << " }\n";
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void CFPermutation::dump() const { print(dbgs(), nullptr); }
#endif

} // namespace snippy
} // namespace llvm
