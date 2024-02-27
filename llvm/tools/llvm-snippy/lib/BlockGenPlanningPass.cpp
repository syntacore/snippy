//===-- BlockGenPlanningPass.cpp ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BlockGenPlanningPass.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Generator/LLVMState.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

#include <functional>

#define DEBUG_TYPE "snippy-block-gen-plan"
#define PASS_DESC "Snippy basic block generation planning"

namespace llvm {
namespace snippy {
namespace {

class BlockGenPlanningImpl {
  GeneratorContext *GenCtx;
  const MachineLoopInfo *MLI;
  std::vector<std::reference_wrapper<const MachineBasicBlock>> BlocksToProcess;

public:
  BlockGenPlanningImpl(GeneratorContext *GenCtxIn, const MachineLoopInfo *MLIIn)
      : GenCtx(GenCtxIn), MLI(MLIIn) {}

  BlocksGenPlanTy processFunction(const MachineFunction &MF);

private:
  BlocksGenPlanTy processFunctionWithNumInstr(const MachineFunction &MF);
  BlocksGenPlanTy processFunctionWithSize(const MachineFunction &MF);
  BlocksGenPlanTy processFunctionMixed(const MachineFunction &MF);

  size_t calculateMFSizeLimit(const MachineFunction &MF) const;

  void fillPlanWithBurstGroups(BlocksGenPlanTy &GenPlan, size_t NumInstrBurst,
                               size_t NumInstrTotal, size_t AverageBlockInstrs);
  void fillPlanWithPlainInstsByNumber(BlocksGenPlanTy &GenPlan,
                                      size_t NumInstrPlain,
                                      size_t AverageBlockInstrs);
  void fillPlanWithPlainInstsBySize(BlocksGenPlanTy &GenPlan,
                                    size_t MFSizeLimit);
  void fillPlanForTopLoopBySize(BlocksGenPlanTy &GenPlan,
                                const MachineLoop &ML) const;
  void updateBlocksToProcess(const SingleBlockGenPlanTy &BlockPlan,
                             size_t AverageBlockInstrs, size_t BlockId);
};

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::BlockGenPlanning;

char BlockGenPlanning::ID = 0;

INITIALIZE_PASS_BEGIN(BlockGenPlanning, DEBUG_TYPE, PASS_DESC, false, true)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_END(BlockGenPlanning, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

MachineFunctionPass *createBlockGenPlanningPass() {
  return new BlockGenPlanning();
}

namespace snippy {

BlockGenPlanning::BlockGenPlanning() : MachineFunctionPass(ID) {
  initializeBlockGenPlanningPass(*PassRegistry::getPassRegistry());
}

StringRef BlockGenPlanning::getPassName() const { return PASS_DESC " Pass"; }

void BlockGenPlanning::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<MachineLoopInfo>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

template <typename IteratorType>
static size_t getCodeSize(IteratorType Begin, IteratorType End) {
  return std::accumulate(Begin, End, 0llu,
                         [](size_t CurrentSize, const auto &MC) {
                           size_t InstrSize = MC.getDesc().getSize();
                           if (InstrSize == 0)
                             errs() << "warning: Instruction has unknown size, "
                                       "the size calculation will be wrong.\n";
                           return CurrentSize + InstrSize;
                         });
}

static size_t getFunctionSize(const MachineFunction &MF) {
  return std::accumulate(MF.begin(), MF.end(), 0llu,
                         [](size_t CurrentSize, const auto &MBB) {
                           auto End = MBB.end();
                           auto Begin = MBB.begin();
                           return CurrentSize + getCodeSize(Begin, End);
                         });
}

size_t
BlockGenPlanningImpl::calculateMFSizeLimit(const MachineFunction &MF) const {
  assert(!GenCtx->isInstrsNumKnown());
  auto OutSectionDesc = GenCtx->getOutputSectionFor(MF);
  auto MaxSize = OutSectionDesc.Size;
  auto CurrentCodeSize = getFunctionSize(MF);

  // last instruction in the trace might be target dependent: EBREAK or
  // int 3, etc.
  auto LastInstr = GenCtx->getLastInstr();
  // If not entry function, we generare ret anyway.
  bool EmptyLastInstr = GenCtx->isEntryFunction(MF) && LastInstr.empty();

  const auto &SnpTgt = GenCtx->getLLVMState().getSnippyTarget();
  auto SizeOfOpc = SnpTgt.getMaxInstrSize();

  // FIXME: lastInstructions == we reserve space to put final instruction
  // and any additional instructions that will be placed after random
  // instructions generation. This should be replaced as we have BlockInfo
  auto SpilledRegs = GenCtx->getSpilledRegs();
  auto NumOfSpilledRegs = SpilledRegs.size();
  // FIXME: may need to generic algorithm.
  size_t SizeForSpilledRegs = NumOfSpilledRegs * 5u * SizeOfOpc;
  // Prologue + Epilogue.
  SizeForSpilledRegs *= 2u;
  size_t SizeForLastInstruction = EmptyLastInstr ? 0u : SizeOfOpc;
  size_t SizeOfLastInstructions = SizeForLastInstruction + SizeForSpilledRegs;

  size_t CodeSizePerFunction =
      MaxSize > CurrentCodeSize ? MaxSize - CurrentCodeSize : 0;

  size_t LocalCodeSizeLimit = 0;
  if (CodeSizePerFunction >= SizeOfLastInstructions)
    LocalCodeSizeLimit = CodeSizePerFunction - SizeOfLastInstructions;
  else
    snippy::warn(WarningName::InstructionCount, GenCtx->getLLVMState().getCtx(),
                 "It seems that the last instruction can not be inserted "
                 "because of size restrictions",
                 "Likely, you need to increase RX section.");

  return LocalCodeSizeLimit;
}

// Collect latch blocks of loops that require special attention. If tracking
// mode is disabled, latch blocks can be treated as ordinary blocks.
static std::unordered_set<const MachineBasicBlock *>
collectLatchBlocks(const GeneratorContext &GenCtx, const MachineLoopInfo &MLI,
                   const MachineFunction &MF) {
  if (!GenCtx.hasTrackingMode())
    return {};

  auto LatchBlocksRange = make_filter_range(MF, [&MLI](const auto &MBB) {
    auto ML = MLI.getLoopFor(&MBB);
    return ML && ML->getLoopLatch() == &MBB;
  });

  std::unordered_set<const MachineBasicBlock *> LatchBlocks;
  transform(LatchBlocksRange, std::inserter(LatchBlocks, LatchBlocks.begin()),
            [](const auto &MBB) { return &MBB; });
  return LatchBlocks;
}

static double getBurstProb(const Config &Cfg, GeneratorContext &GenCtx) {
  auto TotalWeight = Cfg.Histogram.getTotalWeight();
  auto CFWeight = Cfg.Histogram.getCFWeight(GenCtx.getOpcodeCache());
  auto BurstWeight = Cfg.getBurstOpcodesWeight();
  assert(TotalWeight >= CFWeight);
  assert(BurstWeight <= (TotalWeight - CFWeight));
  double BurstProb = (std::abs(TotalWeight - CFWeight) <=
                      std::numeric_limits<double>::epsilon())
                         ? 0.0
                         : BurstWeight / (TotalWeight - CFWeight);
  return BurstProb;
}

// Returns number of instructions that each burst group must have in the
// resulting snippet (mapping from instr count to burst group id). Number of
// instruction for each burst group is calculated in accordance with opcode
// weights from it. Multimap because different burst groups might have the same
// num instrs.
using NumInstrToGroupIdTy = std::multimap<size_t, size_t>;
static NumInstrToGroupIdTy
getBurstInstCounts(GeneratorContext &GenCtx, unsigned long long NumInstrBurst,
                   unsigned long long NumInstrTotal) {
  const auto &BGram = GenCtx.getBurstGram();
  if (!BGram.Groupings)
    return {};
  assert(BGram.Groupings->size() > 0);

  auto OpcodeToNumOfGroups = BGram.getOpcodeToNumBurstGroups();

  const auto &Cfg = GenCtx.getConfig();
  NumInstrToGroupIdTy NumInstrToGroupId;
  auto InstrLeft = NumInstrBurst;
  for (const auto &[Idx, Group] : enumerate(drop_end(*BGram.Groupings))) {
    auto Weight =
        std::accumulate(Group.begin(), Group.end(), 0.0,
                        [&OpcodeToNumOfGroups, &Cfg](double Acc, auto Opcode) {
                          assert(OpcodeToNumOfGroups.count(Opcode));
                          // If an opcode is used more in one burst group, its
                          // weight must be distributed among these groups.
                          return Acc + Cfg.Histogram.weight(Opcode) /
                                           OpcodeToNumOfGroups[Opcode];
                        });

    unsigned long long GroupNumInstrTotal =
        Weight / Cfg.Histogram.getTotalWeight() * NumInstrTotal;
    NumInstrToGroupId.emplace(GroupNumInstrTotal, Idx);
    assert(InstrLeft >= GroupNumInstrTotal);
    InstrLeft -= GroupNumInstrTotal;
  }
  auto Idx = NumInstrToGroupId.size();
  NumInstrToGroupId.emplace(InstrLeft, Idx);
  return NumInstrToGroupId;
}

static size_t extractBurstGroup(NumInstrToGroupIdTy &NumInstrToGroupId,
                                size_t BurstGroupInstCount) {
  // The last group in the NumInstrToGroupId (multi)map has the biggest number
  // of instructions to be added to generation plan. So, process it first as
  // the more instruction count, the higher probability of insertion to
  // generation plan that group has.
  if (NumInstrToGroupId.rbegin()->first >= BurstGroupInstCount) {
    // We must change the key as the group is planned for generation and its
    // instruction number left must be reduced. So extract, change the key and
    // insert.
    auto NH = NumInstrToGroupId.extract(NumInstrToGroupId.rbegin()->first);
    auto GroupId = NH.mapped();
    if (NH.key() != BurstGroupInstCount) {
      NH.key() -= BurstGroupInstCount;
      NumInstrToGroupId.insert(std::move(NH));
    }
    return GroupId;
  }

  // The last group is the biggest one. Start from it.
  auto NH = NumInstrToGroupId.extract(NumInstrToGroupId.rbegin()->first);
  auto GroupId = NH.mapped();
  auto NumInstrAccumulated = NH.key();
  // Don't insert the group to the map as the requested inst count is
  // planned for generation.

  while (NumInstrAccumulated < BurstGroupInstCount) {
    // The group we've chosen above doesn't cover number of instructions to
    // generate, so remove groups with the smallest instruction count left.
    assert(NumInstrToGroupId.size() &&
           "Total number of available instructions in NumInstrToGroupId must "
           "not be smaller than BurstGroupInstCount");
    auto NH = NumInstrToGroupId.extract(NumInstrToGroupId.begin());
    auto N = std::min(NH.key(), BurstGroupInstCount - NumInstrAccumulated);
    NumInstrAccumulated += N;
    if (N < NH.key()) {
      NH.key() -= N;
      NumInstrToGroupId.insert(std::move(NH));
      assert(NumInstrAccumulated == BurstGroupInstCount);
    }
  }
  assert(NumInstrAccumulated == BurstGroupInstCount);

  return GroupId;
}

// Randomly distribute burst groups over generation plan for BBs from
// BlocksToProcess.
//
// Short algo example:
//
// NumInstrBurst is 21, burst group size is 7, AverageBlockInstrs is 5, five BBs
// to fill.
//   NumInstrToGroupId    BlocksToProcess    GenPlan
//     (num of instrs       (BB ->            (BB ->
//      for the group        current size)     packs)
//      -> group id)
//     1: 8 -> id1          BB1 -> 0         BB1 -> empty
//     2: 5 -> id2          BB2 -> 0         BB2 -> empty
//     3: 4 -> id3          BB3 -> 0         BB3 -> empty
//     4: 4 -> id4          BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// At the first iteration we take the first entry from NumInstrToGroupId as it
// has the biggest num instrs and any random BB (e.g. BB3) form BlocksToProcess.
// Then we add one burst group of size 7 with id1 to generation plan for the
// BB. Next step is to update NumInstrToGroupId map: `1: 8 -> id1` -> `1: 1 ->
// id1` as seven instructions were already added to plan.
//
// After the first iteration:
//   NumInstrToGroupId    BlocksToProcess    GenPlan
//     1: 1 -> id1          BB1 -> 0         BB1 -> empty
//     2: 5 -> id2          BB2 -> 0         BB2 -> empty
//     3: 4 -> id3          BB3 -> 7         BB3 -> Burst[7, id1]
//     4: 4 -> id4          BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// Next iteration: we take group 2 as it has the biggest instcount to plan and
// random BB (e.g. BB2). As you can see, group `2: 5 -> id2` doesn't have 7
// instructions, so we'll take five instructions from it and steal additional
// two from other groups. Current algorithm implementation steals instructions
// from groups with the smallest number of instructions to plan. In our case
// they are group 1 and group 3 (or 4, but let's use 3).
//
// After the iteration:
//   NumInstrToGroupId    BlocksToProcess    GenPlan
//    ~1: 0 -> id1~         BB1 -> 0         BB1 -> empty
//    ~2: 0 -> id2~         BB2 -> 7         BB2 -> Burst[7, id2]
//     3: 3 -> id3          BB3 -> 7         BB3 -> Burst[7, id1]
//     4: 4 -> id4          BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// Next iteration. We choose group 4 and BB2 again(random), add three
// instructions from group 3 to it:
//
// After the iteration:
//   NumInstrToGroupId    BlocksToProcess    GenPlan
//    ~1: 0 -> id1~         BB1 -> 0         BB1 -> empty
//    ~2: 0 -> id2~        ~BB2 -> 14~       BB2 -> Burst[7, id2], Burst[7, id4]
//    ~3: 0 -> id3~         BB3 -> 7         BB3 -> Burst[7, id1]
//    ~4: 0 -> id4~         BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// NB: after the last iteration we crossed out BB2 from BlocksToProcess as it
// became bigger than 2 * AverageBlockInstrs. So, no more packs would be added
// to it if we continued. The rule that excludes blocks must be improved, but
// currently it preserves the old behavior.
void BlockGenPlanningImpl::fillPlanWithBurstGroups(BlocksGenPlanTy &GenPlan,
                                                   size_t NumInstrBurst,
                                                   size_t NumInstrTotal,
                                                   size_t AverageBlockInstrs) {
  auto NumInstrToGroupId =
      getBurstInstCounts(*GenCtx, NumInstrBurst, NumInstrTotal);
  const auto &BurstSettings = GenCtx->getBurstGram();
  while (NumInstrBurst > 0) {
    auto BlockId = RandEngine::genInRange(BlocksToProcess.size());
    const auto *MBB = &BlocksToProcess[BlockId].get();

    auto BurstGroupInstCount =
        RandEngine::genInInterval(BurstSettings.MinSize, BurstSettings.MaxSize);
    // The last burst group might be smaller than the minimum size requested in
    // the configuration. This matches the behavior we had before. The
    // difference is that this group can be placed in any random basic block,
    // not in the last block in the function as it was in the previous
    // implementation.
    BurstGroupInstCount =
        std::min<unsigned long long>(BurstGroupInstCount, NumInstrBurst);

    auto GroupId = extractBurstGroup(NumInstrToGroupId, BurstGroupInstCount);

    auto [BlockPlanIt, _] = GenPlan.try_emplace(MBB, GenerationMode::NumInstrs);
    auto &BlockPlan = BlockPlanIt->second;
    BlockPlan.add({BurstGroupInstCount, GroupId});
    NumInstrBurst -= BurstGroupInstCount;

    updateBlocksToProcess(BlockPlan, AverageBlockInstrs, BlockId);
  }
}

void BlockGenPlanningImpl::fillPlanWithPlainInstsByNumber(
    BlocksGenPlanTy &GenPlan, size_t NumInstrPlain, size_t AverageBlockInstrs) {
  while (NumInstrPlain > 0) {
    auto MaxBlockInstrs = AverageBlockInstrs * 2ull;
    auto InstrsToAdd = RandEngine::genInInterval(
        1ull, std::min<unsigned long long>(NumInstrPlain, MaxBlockInstrs));

    auto BlockId = RandEngine::genInRange(BlocksToProcess.size());
    const auto *MBB = &BlocksToProcess[BlockId].get();

    auto [BlockPlanIt, _] = GenPlan.try_emplace(MBB, GenerationMode::NumInstrs);
    auto &BlockPlan = BlockPlanIt->second;
    InstrsToAdd =
        std::min(InstrsToAdd, MaxBlockInstrs - BlockPlan.instrCount());

    BlockPlan.add({InstrsToAdd});

    NumInstrPlain -= InstrsToAdd;

    updateBlocksToProcess(BlockPlan, AverageBlockInstrs, BlockId);
  }
}

void BlockGenPlanningImpl::updateBlocksToProcess(
    const SingleBlockGenPlanTy &BlockPlan, size_t AverageBlockInstrs,
    size_t BlockId) {
  // FIXME: We should make a smarter choice allowing big BBs with a low
  // propability instead of allowing BB sizes only in [0, 2 * Average block
  // size].
  if (BlockPlan.instrCount() >= AverageBlockInstrs * 2) {
    if (BlockId != BlocksToProcess.size() - 1)
      std::swap(BlocksToProcess[BlockId], BlocksToProcess.back());
    BlocksToProcess.pop_back();
  }
}

template <GenerationMode GM, typename T>
void addEmptyPlanForBlocks(BlocksGenPlanTy &GenPlan, const T &Blocks) {
  transform(Blocks, std::inserter(GenPlan, GenPlan.begin()),
            [](const MachineBasicBlock &MBB) {
              return std::make_pair(&MBB, SingleBlockGenPlanTy(GM));
            });
}

BlocksGenPlanTy
BlockGenPlanningImpl::processFunctionWithNumInstr(const MachineFunction &MF) {
  assert(GenCtx->getGenerationMode() == GenerationMode::NumInstrs);

  auto LatchBlocks = collectLatchBlocks(*GenCtx, *MLI, MF);
  std::copy_if(
      MF.begin(), MF.end(), std::back_inserter(BlocksToProcess),
      [&LatchBlocks](const auto &MBB) { return !LatchBlocks.count(&MBB); });
  assert(!BlocksToProcess.empty() &&
         "At least one basic block that is not a latch block must exist");

  auto NumInstrTotal = GenCtx->getRequestedInstrsNum(MF);
  assert(NumInstrTotal >= GenCtx->getCFInstrsNum(MF));
  NumInstrTotal -= GenCtx->getCFInstrsNum(MF);

  const auto &Cfg = GenCtx->getConfig();
  // FIXME: NumInstrBurst should be somehow randomized. But we must be careful
  // as in some cases there are no instructions outside burst groups and then
  // the number must be exact.
  unsigned long long NumInstrBurst = getBurstProb(Cfg, *GenCtx) * NumInstrTotal;
  auto NumInstrPlain = NumInstrTotal - NumInstrBurst;

  auto AverageBlockInstrs = NumInstrTotal / BlocksToProcess.size();
  if (AverageBlockInstrs == 0)
    AverageBlockInstrs = 1;

  BlocksGenPlanTy GenPlan;
  fillPlanWithBurstGroups(GenPlan, NumInstrBurst, NumInstrTotal,
                          AverageBlockInstrs);
  fillPlanWithPlainInstsByNumber(GenPlan, NumInstrPlain, AverageBlockInstrs);

  // Add default plans for remaining blocks.
  addEmptyPlanForBlocks<GenerationMode::NumInstrs>(GenPlan, BlocksToProcess);
  addEmptyPlanForBlocks<GenerationMode::NumInstrs>(
      GenPlan,
      map_range(LatchBlocks,
                [](const MachineBasicBlock *MBB) -> const MachineBasicBlock & {
                  return *MBB;
                }));

  // Randomize generation plan: shuffle burst groups and plain instructions.
  for (auto &BlockPlan : make_second_range(GenPlan))
    BlockPlan.randomize();

  return GenPlan;
}

void BlockGenPlanningImpl::fillPlanWithPlainInstsBySize(
    BlocksGenPlanTy &GenPlan, size_t MFSizeLimit) {
  auto MaxInstrSize =
      GenCtx->getLLVMState().getSnippyTarget().getMaxInstrSize();
  // Multiset instead of vector because we generate accumulated size of first k
  // blocks. For example, if PlannedAccumulatedSizes == {2, 4, 4, 25, 37} then
  // blocks will generated with {2, 2, 0, 21, 12} sizes.
  std::multiset<size_t> PlannedAccumulatedSizes = {MFSizeLimit};
  transform(
      seq(0ul, BlocksToProcess.size() - 1),
      std::inserter(PlannedAccumulatedSizes, PlannedAccumulatedSizes.end()),
      [=](auto) {
        return RandEngine::genInInterval(MFSizeLimit) / MaxInstrSize *
               MaxInstrSize;
      });

  size_t LastAccumulatedSize = 0;
  for (auto [MBB, AccumulatedSize] :
       zip(BlocksToProcess, PlannedAccumulatedSizes)) {
    auto [BlockPlanIt, _] =
        GenPlan.try_emplace(&MBB.get(), GenerationMode::Size);
    auto &BlockPlan = BlockPlanIt->second;
    size_t BlockSize = AccumulatedSize - LastAccumulatedSize;
    BlockPlan.add({BlockSize});
    LastAccumulatedSize = AccumulatedSize;
  }
}

BlocksGenPlanTy
BlockGenPlanningImpl::processFunctionWithSize(const MachineFunction &MF) {
  assert(GenCtx->getGenerationMode() == GenerationMode::Size);

  std::copy(MF.begin(), MF.end(), std::back_inserter(BlocksToProcess));
  assert(!BlocksToProcess.empty() && "At least one basic block must exist");

  auto MFSizeLimit = calculateMFSizeLimit(MF);

  BlocksGenPlanTy GenPlan;
  fillPlanWithPlainInstsBySize(GenPlan, MFSizeLimit);

  return GenPlan;
}

static auto accumulateMISize(unsigned long long Acc, const MachineInstr &MI) {
  auto InstrSize = MI.getDesc().getSize();
  assert(InstrSize != 0 && "Instruction with unknown size is unsupported");
  return Acc + InstrSize;
}

template <typename T> static auto getSize(T First, T Last) {
  return std::accumulate(First, Last, 0ull, &accumulateMISize);
}

static auto getMBBSize(const MachineBasicBlock &MBB) {
  return getSize(MBB.begin(), MBB.end());
}

static size_t calcFilledSize(const BlocksGenPlanTy &GenPlan,
                             ArrayRef<const MachineBasicBlock *> Blocks) {
  size_t FilledSize = 0;
  for (auto *Block : Blocks) {
    if (GenPlan.count(Block))
      FilledSize += GenPlan.at(Block).size() + getMBBSize(*Block);
    else
      FilledSize += getMBBSize(*Block);
  }
  return FilledSize;
}

static void setSizeForLoopBlock(BlocksGenPlanTy &GenPlan,
                                const MachineBasicBlock &SelectedMBB,
                                ArrayRef<const MachineBasicBlock *> LoopBlocks,
                                NumericRange<ProgramCounterType> PCDist,
                                bool IsLatch, GeneratorContext &SGCtx) {
  assert(!GenPlan.count(&SelectedMBB));
  auto &SnpTgt = SGCtx.getLLVMState().getSnippyTarget();
  auto BrOpc = SelectedMBB.getFirstTerminator()->getOpcode();
  auto MaxBranchDstMod = SnpTgt.getMaxBranchDstMod(BrOpc);
  if (PCDist.Max.has_value() && PCDist.Max.value() > MaxBranchDstMod) {
    auto OpName = SGCtx.getOpcodeCache().name(BrOpc);
    snippy::notice(WarningName::TooFarMaxPCDist,
                   SelectedMBB.getParent()->getFunction().getContext(),
                   "Specified max PC Distance is more than max distance for "
                   "generated branch",
                   "Specified: " + Twine(PCDist.Max.value()) +
                       ", max distance for " + OpName + ": " +
                       Twine(MaxBranchDstMod));
    PCDist.Max = MaxBranchDstMod;
  }
  if (!PCDist.Max.has_value())
    PCDist.Max = MaxBranchDstMod;

  size_t FilledSize = calcFilledSize(GenPlan, LoopBlocks);
  if (IsLatch) // Branches size isn't included in backward distance
    FilledSize -= getSize(SelectedMBB.getFirstTerminator(), SelectedMBB.end());

  if (PCDist.Max.value() < FilledSize)
    snippy::fatal(SelectedMBB.getParent()->getFunction().getContext(),
                  "Max PC distance requirement can't be met",
                  "Loop is already filled with " + Twine(FilledSize) +
                      " bytes, but max pc distance is " +
                      Twine(PCDist.Max.value()));

  NumericRange<unsigned> BlockRange;
  BlockRange.Max = PCDist.Max.value() - FilledSize;
  if (PCDist.Min.has_value())
    BlockRange.Min =
        (PCDist.Min.value() > FilledSize) ? PCDist.Min.value() - FilledSize : 0;

  auto MaxInstrSize = SnpTgt.getMaxInstrSize();
  auto Min = alignTo(BlockRange.Min.value_or(0), MaxInstrSize);
  auto Max = alignDown(BlockRange.Max.value(), MaxInstrSize);
  if (Min > Max)
    snippy::fatal(SelectedMBB.getParent()->getFunction().getContext(),
                  "Max PC distance requirement can't be met",
                  "Min distance is " + Twine(Min) + " , but max distance is " +
                      Twine(Max));

  auto BlockSize = RandEngine::genInInterval(Min, Max);
  BlockSize = alignDown(BlockSize, MaxInstrSize);

  auto [BlockPlanIt, Emplaced] =
      GenPlan.try_emplace(&SelectedMBB, GenerationMode::Size);
  assert(Emplaced);
  auto &BlockPlan = BlockPlanIt->second;
  BlockPlan.add({BlockSize});
}

void BlockGenPlanningImpl::fillPlanForTopLoopBySize(
    BlocksGenPlanTy &GenPlan, const MachineLoop &ML) const {
  assert(ML.isOutermost() && "Only top level loop expected");
  if (!ML.getSubLoops().empty())
    fatal(GenCtx->getLLVMState().getCtx(), "Block generation planning failed",
          "PC distance is now supported with max loop depth 1");

  auto PCDist = GenCtx->getConfig().Branches.getPCDistance();

  auto LoopBlocks = ML.getBlocks();
  for (auto *MBB : LoopBlocks)
    setSizeForLoopBlock(GenPlan, *MBB, LoopBlocks, PCDist, ML.isLoopLatch(MBB),
                        *GenCtx);
}

BlocksGenPlanTy
BlockGenPlanningImpl::processFunctionMixed(const MachineFunction &MF) {
  assert(GenCtx->getGenerationMode() == GenerationMode::Mixed);

  BlocksGenPlanTy GenPlan;
  unsigned SupposedNumInstr = 0;
  auto MaxInstrSize =
      GenCtx->getLLVMState().getSnippyTarget().getMaxInstrSize();
  for (auto *ML : *MLI) {
    assert(ML);
    fillPlanForTopLoopBySize(GenPlan, *ML);
    for (auto *MBB : ML->blocks()) {
      auto BBSize = GenPlan.at(MBB).size();
      SupposedNumInstr += BBSize / MaxInstrSize;
      SupposedNumInstr += BBSize % MaxInstrSize ? 1 : 0;
    }
  }

  // Process blocks out of loops

  std::copy_if(MF.begin(), MF.end(), std::back_inserter(BlocksToProcess),
               [this](const auto &MBB) { return !MLI->getLoopFor(&MBB); });

  auto NumInstrTotal = GenCtx->getRequestedInstrsNum(MF);
  assert(NumInstrTotal >= GenCtx->getCFInstrsNum(MF));
  NumInstrTotal -= GenCtx->getCFInstrsNum(MF);
  // If number of instructions in size-requested blocks is already enough for
  // the whole function, skipping num instrs planning for other blocks
  if (NumInstrTotal <= SupposedNumInstr) {
    addEmptyPlanForBlocks<GenerationMode::NumInstrs>(GenPlan, BlocksToProcess);
    return GenPlan;
  }

  NumInstrTotal -= SupposedNumInstr;

  const auto &Cfg = GenCtx->getConfig();
  // FIXME: NumInstrBurst should be somehow randomized. But we must be careful
  // as in some cases there are no instructions outside burst groups and then
  // the number must be exact.
  unsigned long long NumInstrBurst = getBurstProb(Cfg, *GenCtx) * NumInstrTotal;
  auto NumInstrPlain = NumInstrTotal - NumInstrBurst;

  auto AverageBlockInstrs = NumInstrTotal / BlocksToProcess.size();
  if (AverageBlockInstrs == 0)
    AverageBlockInstrs = 1;

  fillPlanWithBurstGroups(GenPlan, NumInstrBurst, NumInstrTotal,
                          AverageBlockInstrs);
  fillPlanWithPlainInstsByNumber(GenPlan, NumInstrPlain, AverageBlockInstrs);

  // Add default plans for remaining blocks.
  addEmptyPlanForBlocks<GenerationMode::NumInstrs>(GenPlan, BlocksToProcess);

  return GenPlan;
}

static void checkGenModeCompatibility(GeneratorContext &GenCtx,
                                      const MachineLoopInfo &MLI) {
  auto GM = GenCtx.getGenerationMode();
  if (GM == GenerationMode::NumInstrs)
    return;

  bool LoopGenerated = !MLI.empty();
  bool TrackingEnabled = GenCtx.hasTrackingMode();
  if (LoopGenerated && TrackingEnabled)
    report_fatal_error(
        "Generation by size with loops in tracking mode is not supported",
        false);
}

BlocksGenPlanTy
BlockGenPlanningImpl::processFunction(const MachineFunction &MF) {
  assert(GenCtx && MLI);
  checkGenModeCompatibility(*GenCtx, *MLI);
  switch (GenCtx->getGenerationMode()) {
  case GenerationMode::NumInstrs:
    return processFunctionWithNumInstr(MF);
  case GenerationMode::Size:
    return processFunctionWithSize(MF);
  case GenerationMode::Mixed:
    return processFunctionMixed(MF);
  }
  llvm_unreachable("unknown generation mode");
}

const SingleBlockGenPlanTy &
BlockGenPlanning::get(const MachineBasicBlock &MBB) const {
  assert(Plan.count(&MBB));
  return Plan.at(&MBB);
}

bool BlockGenPlanning::runOnMachineFunction(MachineFunction &MF) {
  assert(Plan.empty());
  auto *GenCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  auto *MLI = &getAnalysis<MachineLoopInfo>();

  BlockGenPlanningImpl Impl(GenCtx, MLI);
  Plan = Impl.processFunction(MF);

  return true;
}

void BlockGenPlanning::releaseMemory() { Plan.clear(); }

} // namespace snippy
} // namespace llvm
