//===-- CFPermutation.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Generator/CFPermutation.h"
#include "snippy/Config/Branchegram.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"

#define DEBUG_TYPE "snippy-cf-permutation"

namespace llvm {
namespace snippy {
extern cl::OptionCategory Options;
} // namespace snippy
} // namespace llvm

namespace {
using namespace llvm;
using namespace snippy;

snippy::opt<bool>
    PermutationProgress("permutation-progress",
                        cl::desc("Show branch permutation progress."),
                        cl::cat(Options), cl::init(false), cl::Hidden);

snippy::opt<bool> PermutationStatus("permutation-status",
                                    cl::desc("Show branch permutation status."),
                                    cl::cat(Options), cl::init(false),
                                    cl::Hidden);

[[nodiscard]] bool checkBranchSettings(const Branchegram &Branches) {
  return !Branches.anyConsecutiveLoops() ||
         (Branches.LoopRatio == 1.0 && Branches.getMaxLoopDepth() == 1 &&
          Branches.getBlockDistance().Min.value_or(0) == 0 &&
          Branches.getBlockDistance().Max.value_or(0) == 0);
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
                  const CFPermutationContext::BlockInfo::SetT &Available) {
  OS << "Initializing BlocksInfo for BB#" << BB << " with set: {";
  for (auto &&Candidtate : Available)
    OS << Candidtate << ',';
  OS << "}\n";
}

auto calcCandidatesDistribution(unsigned BackwardCount, unsigned ForwardCount,
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

unsigned selectBB(const CFPermutationContext::BlockInfo::SetT &Available,
                  unsigned CurrBB, double LoopRatio) {
  auto BackwardCount =
      count_if(Available, [CurrBB](auto Other) { return Other <= CurrBB; });
  auto ForwardCount = Available.size() - BackwardCount;

  auto DD = calcCandidatesDistribution(BackwardCount, ForwardCount, LoopRatio);
  return RandEngine::selectFromContainer(Available, DD);
}

using CFPermSetT = CFPermutationContext::BlockInfo::SetT;
void eraseRangeFromSet(CFPermSetT &Set,
                       typename CFPermSetT::value_type LowerBound,
                       typename CFPermSetT::value_type UpperBound) {
  auto Beg = Set.lower_bound(LowerBound);
  auto End = Set.upper_bound(UpperBound);
  Set.erase(Beg, End);
}

using CFPermBIIter = CFPermutationContext::BlocksInfoIter;
void preserveDepth(CFPermBIIter Pos, CFPermBIIter Beg, CFPermBIIter End,
                   unsigned LowerBound, unsigned UpperBound) {
  std::for_each(Beg, End, [LowerBound, UpperBound](auto &BI) {
    eraseRangeFromSet(BI.Available, LowerBound, UpperBound);
  });
  Pos->Available.clear();
}

// Simple function for finding if max depth reached
CFPermBIIter
findMaxDepthReachedImpl(CFPermBIIter Beg, CFPermBIIter End,
                        typename CFPermutationContext::BlocksInfoT &BlocksInfo,
                        unsigned CFPermutationContext::BlockInfo::*DepthToCheck,
                        unsigned DepthLimit) {
  assert(Beg <= End && "Respect iteration order");
  return std::find_if(Beg, End, [=, &BlocksInfo](const auto &BI) {
    assert(BI.*DepthToCheck <= DepthLimit &&
           "Depth expected to be not more than MaxDepth");
    bool Reached = BI.*DepthToCheck == DepthLimit;
    if (Reached) {
      LLVM_DEBUG(dbgs() << "MaxDepth reached for BB#"
                        << &BI - BlocksInfo.begin() << '\n');
    }
    return Reached;
  });
}
} // namespace

llvm::snippy::CFPermutationContext::CFPermutationContext(
    MachineFunction &MF, GeneratorContext &GC, FunctionGenerator &FG,
    ConsecutiveLoopInfo &CLI, const SimulatorContext &SimCtx)
    : BlocksInfo(), CurrMF(MF), GC(GC), FG(FG), CLI(CLI), SimCtx(SimCtx),
      BranchSettings(GC.getConfig().Branches) {
  auto &ProgCtx = GC.getProgramContext();

  if (SimCtx.hasTrackingMode() &&
      GC.getGenSettings().isLoopGenerationPossible(ProgCtx.getOpcodeCache()) &&
      !SimCtx.getSimRunner().getPrimaryInterpreter().modelSupportCallbacks())
    fatal(ProgCtx.getLLVMState().getCtx(),
          "Loops cannot be generated in selfcheck/backtrack/hazard modes",
          "Selected model does not support it.");

  LLVM_DEBUG(dbgs() << "CFPermutationContext created for branchegram:\n");
  LLVM_DEBUG(BranchSettings.get().dump());
}

size_t llvm::snippy::CFPermutationContext::getCFInstrNumFor(
    const MachineFunction &MF) const {
  auto TotalInstrNum = FG.get().getRequestedInstrsNum(MF);
  auto &GenSettings = GC.get().getGenSettings();
  auto &ProgCtx = GC.get().getProgramContext();
  auto &OpCC = ProgCtx.getOpcodeCache();
  return GenSettings.getCFInstrsNum(OpCC, TotalInstrNum);
}

bool llvm::snippy::CFPermutationContext::makePermutationAndUpdateBranches() {
  auto CFInstrsNum = getCFInstrNumFor(CurrMF);
  if (CFInstrsNum == 0) {
    LLVM_DEBUG(dbgs() << "No CF instructions for " << CurrMF.get().getName()
                      << "\n");
    return false;
  }

  assert(CurrMF.get().size() - 1 == CFInstrsNum &&
         "MF expected to have all blocks");
  initBlocksInfo(CFInstrsNum);
  auto PermutationOrder = generatePermutationOrder(CFInstrsNum);
  makePermutation(PermutationOrder);
  return updateBranches();
}

MachineBasicBlock *
llvm::snippy::CFPermutationContext::getBlockNumbered(unsigned N) {
  return CurrMF.get().getBlockNumbered(N);
}

const MachineBasicBlock *
llvm::snippy::CFPermutationContext::getBlockNumbered(unsigned N) const {
  return CurrMF.get().getBlockNumbered(N);
}

unsigned calcMaxBBDst(unsigned BB, unsigned NBlocks,
                      unsigned RequestedInstrsNum, unsigned Opcode,
                      bool DoAutoMaxBBDistance, const Branchegram &BS,
                      GeneratorContext &GC) {
  LLVM_DEBUG(dbgs() << "Initializing BlocksInfo for " << NBlocks
                    << " blocks\n");
  LLVM_DEBUG(dbgs() << (DoAutoMaxBBDistance ? "" : "don't ")
                    << "calculate max distance adaptively\n");
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto MaxDstMod = State.getSnippyTarget().getMaxBranchDstMod(Opcode);
  auto AutoMaxDistance = CFPermutationContext::calculateMaxDistance(
      BB, NBlocks, RequestedInstrsNum, MaxDstMod, GC);
  unsigned MaxBBDst =
      DoAutoMaxBBDistance ? AutoMaxDistance : BS.getBlockDistance().Max.value();
  if (MaxBBDst > static_cast<unsigned>(std::numeric_limits<int>::max()))
    fatal(State.getCtx(), "Max block distance doesn't fit int",
          Twine(MaxBBDst));
  return MaxBBDst;
}

auto calcBlocksLimit(unsigned BB, unsigned NBlocks, bool DoAutoMaxBBDistance,
                     unsigned MaxBBDst, const Branchegram &BS) {
  // Loop branch has two more blocks with instructions between source and
  // destination PC comparing with if branch (it is first and last blocks).
  // Thus we need to decrease max distance of backward branch.
  int BackwardDst =
      std::min<int>(BB + 1, BB - MaxBBDst + (DoAutoMaxBBDistance ? 2 : 0));
  unsigned BegLimit = std::max<int>(0, BackwardDst);
  unsigned EndLimit = BB + MaxBBDst;

  // We don't generate loops
  if (BS.LoopRatio == 0 || (BS.getMaxLoopDepth() == 0))
    BegLimit = BB + 1;

  // We generate only loops
  if (BS.LoopRatio == 1 || (BS.hasMaxIfDepth() && BS.getMaxIfDepth() == 0))
    EndLimit = BB;

  return std::make_pair(BegLimit, EndLimit);
}

void llvm::snippy::CFPermutationContext::initOneBlockInfo(
    unsigned BB, unsigned NBlocks, size_t RequestedInstrsNum) {
  assert(BlocksInfo.size() == BB);
  auto *BBPtr = getBlockNumbered(BB);
  assert(BBPtr);
  auto Branch = BBPtr->getFirstTerminator();
  assert(Branch != BBPtr->end());
  // We don't want want to permute unconditional branches
  if (Branch->isUnconditionalBranch()) {
    BlocksInfo.emplace_back(BB + 1, BlockInfo::SetT());
    return;
  }

  auto &BS = BranchSettings.get();
  bool DoAutoMaxBBDistance = !BS.getBlockDistance().Max.has_value();
  unsigned MinBBDst = BS.getBlockDistance().Min.value_or(0);
  unsigned MaxBBDst =
      calcMaxBBDst(BB, NBlocks, RequestedInstrsNum, Branch->getOpcode(),
                   DoAutoMaxBBDistance, BS, GC.get());
  auto [BegLimit, EndLimit] =
      calcBlocksLimit(BB, NBlocks, DoAutoMaxBBDistance, MaxBBDst, BS);

  // Setting initial available set for BB as
  // [BB - MaxDst, BB - MinDst] U (BB + MinDst, BB + MaxDst]
  // to preserve min and max distance
  unsigned BackwardSeqBeg = BegLimit;
  unsigned BackwardSeqEnd = BB < MinBBDst ? 0 : BB - MinBBDst;
  bool BackwardPossible = BB >= MinBBDst && BackwardSeqBeg <= BackwardSeqEnd;
  unsigned ForwardSeqBeg = BB + std::max(MinBBDst, 1u);
  unsigned ForwardSeqEnd = std::min(NBlocks, EndLimit);
  bool ForwardPossible = ForwardSeqBeg <= ForwardSeqEnd;

  LLVM_DEBUG(printRangesForAvailableSet(
      dbgs(), BackwardPossible, BackwardSeqBeg, BackwardSeqEnd, ForwardPossible,
      ForwardSeqBeg, ForwardSeqEnd));

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

void llvm::snippy::CFPermutationContext::initBlocksInfo(unsigned Size) {
  auto &BS = BranchSettings.get();
  BlocksInfo.reserve(Size);
  assert(checkBranchSettings(BS) && "unsupported branch settings");
  if (BS.onlyConsecutiveLoops()) {
    transform(seq(0u, Size), std::back_inserter(BlocksInfo),
              [](unsigned BB) { return BlockInfo(BB); });
    for (auto &&BB : seq(1u, Size))
      CLI.get().registerConsecutiveLoopsHeader(BB, 0u);
    return;
  }

  if (BS.anyConsecutiveLoops()) {
    transform(seq(0u, Size), std::back_inserter(BlocksInfo), [](unsigned BB) {
      return BlockInfo(BB + 1, BlockInfo::SetT({BB, BB + 1}));
    });
    return;
  }

  auto RequestedInstrsNum = FG.get().getRequestedInstrsNum(CurrMF);

  for (auto BB : seq(0u, Size))
    initOneBlockInfo(BB, Size, RequestedInstrsNum);
}

unsigned llvm::snippy::CFPermutationContext::calculateMaxDistance(
    unsigned BBNum, unsigned Size, unsigned RequestedInstrsNum,
    unsigned MaxBranchDstMod, GeneratorContext &GC) {
  LLVM_DEBUG(dbgs() << "Calculating max distance for BB#" << BBNum << '\n');
  auto &BS = GC.getConfig().Branches;
  if (BS.anyConsecutiveLoops())
    return 0;
  auto &ProgCtx = GC.getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto PCDist = BS.getPCDistance();
  MaxBranchDstMod = PCDist.Max.value_or(MaxBranchDstMod);
  auto MaxInstrSize = SnippyTgt.getMaxInstrSize();
  if (MaxBranchDstMod < MaxInstrSize)
    snippy::fatal(ProgCtx.getLLVMState().getCtx(),
                  "Max PC distance (" + Twine(MaxBranchDstMod) +
                      ") is less than max instruction size",
                  Twine(MaxInstrSize));

  double AverageBBNInstrs = RequestedInstrsNum / static_cast<double>(Size);
  // NOTE: we can have other overhead, for example, load/store. May be it worth
  // to consider them too.
  unsigned LoopOverhead = SnippyTgt.getLoopOverhead();
  // NLoops = LoopRatio * Size
  // AverageLoopOverheadPerBB = LoopOverhead * NLoops / Size;
  // AverageLoopOverheadPerBB = LoopOverhead * LoopRatio * Size / Size;
  double AverageLoopOverheadPerBB = LoopOverhead * BS.LoopRatio;
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

SmallVector<unsigned>
llvm::snippy::CFPermutationContext::generatePermutationOrder(unsigned Size) {
  auto Seq = seq(0u, Size);
  SmallVector<unsigned> Order(Seq.begin(), Seq.end());
  RandEngine::shuffle(Order.begin(), Order.end());

  LLVM_DEBUG(dbgs() << "Permutation order:\n");
  LLVM_DEBUG(dbgs() << Order.front());
  LLVM_DEBUG(
      for_each(drop_begin(Order), [](auto BB) { dbgs() << ", " << BB; }));
  LLVM_DEBUG(dbgs() << '\n');

  return Order;
}

void llvm::snippy::CFPermutationContext::makePermutation(
    ArrayRef<unsigned> PermutationOrder) {
  LLVM_DEBUG(dbgs() << "Available sets before permutation:\n");
  LLVM_DEBUG(dump());

  PermutationCounter = 0;

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

void llvm::snippy::CFPermutationContext::dumpPermutedBranchIfNeeded(
    unsigned BB, bool IsLoop, const BlockInfo &BI) const {
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
    LLVM_DEBUG(getBlockNumbered(BB)->getFirstTerminator()->dump());
  }
}

void llvm::snippy::CFPermutationContext::permuteBlock(unsigned BB) {
  assert(BB < BlocksInfo.size() && "BB out of range");
  LLVM_DEBUG(dbgs() << "Permuting BB#" << BB << '\n');

  auto BI = std::next(BlocksInfo.begin(), BB);
  if (BI->Available.empty()) {
    LLVM_DEBUG(dbgs() << "Don't permute BB#" << BB
                      << " because available set is empty\n");
    return;
  }

  auto &BS = BranchSettings.get();
  unsigned NConsecutiveLoops = BS.getNConsecutiveLoops();
  // We cannot always create N consecutive loops
  auto BIEnd = std::min(BI + NConsecutiveLoops + 1, BlocksInfo.end());
  auto NextCantBeSingleBlockLoopPos =
      std::find_if(BI, BIEnd, [this](const auto &NextBI) {
        if (NextBI.Available.empty())
          return true;
        return NextBI.Available.count(&NextBI - BlocksInfo.data()) == 0;
      });
  NConsecutiveLoops = (NextCantBeSingleBlockLoopPos > BI)
                          ? (NextCantBeSingleBlockLoopPos - BI - 1)
                          : 0;
  unsigned Selected =
      (NConsecutiveLoops == 0) ? selectBB(BI->Available, BB, BS.LoopRatio) : BB;
  bool IsLoop = BB >= Selected;
  BI->Successor = Selected;
  // Clear available set since we already choosed destination and don't need to
  // track requirements for this BB
  BI->Available.clear();

  dumpPermutedBranchIfNeeded(BB, IsLoop, *BI);
  updateBlocksInfo(BB, Selected);
  LLVM_DEBUG(dbgs() << "Available sets after this permutation:\n");
  LLVM_DEBUG(dump());

  if (IsLoop && NConsecutiveLoops > 0) {
    for (auto NextBB : seq_inclusive(BB + 1, BB + NConsecutiveLoops)) {
      LLVM_DEBUG(dbgs() << "Permuting BB#" << NextBB
                        << " (consecutive loop)\n");
      CLI.get().registerConsecutiveLoopsHeader(NextBB, BB);
      auto &NextBI = BlocksInfo[NextBB];
      assert(NextBI.Available.count(NextBB));
      NextBI.Successor = NextBB;
      NextBI.Available.clear();
      dumpPermutedBranchIfNeeded(NextBB, /* IsLoop */ true, NextBI);
      updateBlocksInfo(NextBB, NextBB);
      LLVM_DEBUG(
          dbgs() << "Available sets after consecutive loop permutation:\n");
      LLVM_DEBUG(dump());
    }
  }
}

void llvm::snippy::CFPermutationContext::updateBlocksInfo(unsigned From,
                                                          unsigned To) {
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

CFPermutationContext::BlocksInfoIter
llvm::snippy::CFPermutationContext::findMaxIfDepthReached(BlocksInfoIter Beg,
                                                          BlocksInfoIter End) {
  if (!BranchSettings.get().hasMaxIfDepth())
    return End;
  return findMaxDepthReachedImpl(Beg, End, BlocksInfo, &BlockInfo::IfDepth,
                                 BranchSettings.get().getMaxIfDepth());
}

CFPermutationContext::BlocksInfoIter
llvm::snippy::CFPermutationContext::findMaxLoopDepthReached(
    BlocksInfoIter Beg, BlocksInfoIter End) {
  return findMaxDepthReachedImpl(Beg, End, BlocksInfo, &BlockInfo::LoopDepth,
                                 BranchSettings.get().getMaxLoopDepth());
}

bool llvm::snippy::CFPermutationContext::updateBranches() {
  const auto &ProgCtx = GC.get().getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  bool Changed = false;
  for (auto BBNum : seq<unsigned>(0, BlocksInfo.size())) {
    auto *BB = getBlockNumbered(BBNum);
    assert(BB);
    auto BranchPos = BB->getFirstTerminator();
    assert(BranchPos != BB->end());
    auto &Branch = *BranchPos;
    auto SuccNum = BlocksInfo[BBNum].Successor;
    assert((SuccNum > BBNum || Branch.isConditionalBranch()) &&
           "Loop are not supported for contidional branches");
    auto *NewDest = getBlockNumbered(SuccNum);
    assert(NewDest);
    Changed |= SnippyTgt.replaceBranchDest(Branch, *NewDest);
  }

  return Changed;
}

void llvm::snippy::CFPermutationContext::clear() {
  PermutationCounter = 0;
  BlocksInfo.clear();
}

void llvm::snippy::CFPermutationContext::print(raw_ostream &OS) const {
  for (auto BB : seq<unsigned>(0, BlocksInfo.size())) {
    OS << BB << " : " << BlocksInfo[BB].Successor << " : "
       << BlocksInfo[BB].IfDepth << " : " << BlocksInfo[BB].LoopDepth << " : {";
    for_each(BlocksInfo[BB].Available,
             [&OS](auto Available) { OS << ' ' << Available << ","; });
    OS << " }\n";
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void llvm::snippy::CFPermutationContext::dump() const {
  print(dbgs());
}
#endif
