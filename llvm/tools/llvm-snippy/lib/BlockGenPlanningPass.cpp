//===-- BlockGenPlanningPass.cpp ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BlockGenPlanningPass.h"
#include "InitializePasses.h"

#include "snippy/Generator/BlockGenPlanWrapperPass.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"

#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <functional>

#define DEBUG_TYPE "snippy-block-gen-plan"
#define PASS_DESC "Snippy basic block generation planning"

namespace llvm {
namespace snippy {
namespace {

class BlockGenPlanningImpl {
  GeneratorContext *GenCtx;
  const MachineLoopInfo *MLI;
  const FunctionGenerator *FG;
  SimulatorContext SimCtx;
  std::vector<const MachineBasicBlock *> BlocksToProcess;

public:
  BlockGenPlanningImpl(GeneratorContext *GenCtxIn, const MachineLoopInfo *MLIIn,
                       const FunctionGenerator *FGIn, SimulatorContext SimCtx)
      : GenCtx(GenCtxIn), MLI(MLIIn), FG(FGIn), SimCtx(std::move(SimCtx)) {}

  planning::FunctionRequest processFunction(const MachineFunction &MF);

private:
  planning::FunctionRequest
  processFunctionWithNumInstr(const MachineFunction &MF);
  planning::FunctionRequest processFunctionWithSize(const MachineFunction &MF);
  planning::FunctionRequest processFunctionMixed(const MachineFunction &MF);

  size_t calculateMFSizeLimit(const MachineFunction &MF) const;

  void fillReqWithBurstGroups(planning::FunctionRequest &FunReq,
                              size_t NumInstrBurst, size_t NumInstrTotal,
                              size_t AverageBlockInstrs);

  void fillReqWithPlainInstsByNumber(planning::FunctionRequest &FunReq,
                                     size_t NumInstrPlain,
                                     size_t AverageBlockInstrs);
  void fillReqWithPlainInstsBySize(planning::FunctionRequest &FunReq,
                                   size_t MFSizeLimit);
  void fillReqForTopLoopBySize(planning::FunctionRequest &FunReq,
                               const MachineLoop &ML) const;
  void updateBlocksToProcess(const planning::BasicBlockRequest &BlockReq,
                             size_t AverageBlockInstrs);
  template <typename Predicate>
  void fillBlocksToProcess(const MachineFunction &MF,
                           planning::FunctionRequest &FunReq, Predicate &&Pred);
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
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(BlockGenPlanWrapper)
INITIALIZE_PASS_DEPENDENCY(FunctionGenerator)
INITIALIZE_PASS_END(BlockGenPlanning, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

MachineFunctionPass *createBlockGenPlanningPass() {
  return new BlockGenPlanning();
}

namespace snippy {

StringRef BlockGenPlanning::getPassName() const { return PASS_DESC " Pass"; }

void BlockGenPlanning::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<SimulatorContextWrapper>();
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addRequired<BlockGenPlanWrapper>();
  AU.addRequired<FunctionGenerator>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

size_t
BlockGenPlanningImpl::calculateMFSizeLimit(const MachineFunction &MF) const {
  assert(!GenCtx->getGenSettings().isInstrsNumKnown());
  auto OutSectionDesc = GenCtx->getProgramContext().getOutputSectionFor(MF);
  auto MaxSize = OutSectionDesc.Size;
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnpTgt = State.getSnippyTarget();
  auto CurrentCodeSize = State.getFunctionSize(MF);
  const auto &GenSettings = GenCtx->getGenSettings();
  // last instruction in the trace might be target dependent: EBREAK or
  // int 3, etc.
  auto LastInstr = GenSettings.getLastInstr();
  // If not entry function, we generate ret anyway.
  bool EmptyLastInstr = FG->isEntryFunction(MF) && LastInstr.empty();
  auto SizeOfOpc = SnpTgt.getMaxInstrSize();

  // FIXME: lastInstructions == we reserve space to put final instruction
  // and any additional instructions that will be placed after random
  // instructions generation. This should be replaced as we have BlockInfo
  auto RegsSpilledToStack = GenSettings.getRegsSpilledToStack();
  auto RegsSpilledToMem = GenSettings.getRegsSpilledToMem();
  auto NumOfSpilledRegs = RegsSpilledToStack.size() + RegsSpilledToMem.size();
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
    snippy::warn(WarningName::InstructionCount, ProgCtx.getLLVMState().getCtx(),
                 "It seems that the last instruction can not be inserted "
                 "because of size restrictions",
                 "Likely, you need to increase RX section.");

  return LocalCodeSizeLimit;
}

// Collect latch blocks of loops that require special attention. If tracking
// mode is disabled, latch blocks can be treated as ordinary blocks.
static std::unordered_set<const MachineBasicBlock *>
collectLatchBlocks(const GeneratorContext &GenCtx, const MachineLoopInfo &MLI,
                   const MachineFunction &MF, SimulatorContext &SimCtx) {
  if (!SimCtx.hasTrackingMode())
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
  auto CFWeight =
      Cfg.Histogram.getCFWeight(GenCtx.getProgramContext().getOpcodeCache());
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
  const auto &BGram = GenCtx.getGenSettings().getBurstGram();
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
//   NumInstrToGroupId    BlocksToProcess    FunReq
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
//   NumInstrToGroupId    BlocksToProcess    FunReq
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
//   NumInstrToGroupId    BlocksToProcess    FunReq
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
//   NumInstrToGroupId    BlocksToProcess    FunReq
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
void BlockGenPlanningImpl::fillReqWithBurstGroups(
    planning::FunctionRequest &FunReq, size_t NumInstrBurst,
    size_t NumInstrTotal, size_t AverageBlockInstrs) {
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &GenSettings = GenCtx->getGenSettings();
  auto NumInstrToGroupId =
      getBurstInstCounts(*GenCtx, NumInstrBurst, NumInstrTotal);
  const auto &BurstSettings = GenSettings.getBurstGram();
  while (NumInstrBurst > 0) {
    auto BlockId = RandEngine::genInRange(BlocksToProcess.size());
    const auto *MBB = BlocksToProcess[BlockId];

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

    FunReq.addToBlock(
        MBB, planning::InstructionGroupRequest(
                 planning::RequestLimit::NumInstrs{BurstGroupInstCount},
                 planning::BurstGenPolicy(ProgCtx, GenSettings, GroupId)));
    NumInstrBurst -= BurstGroupInstCount;

    auto &BlockReq = FunReq.at(MBB);
    updateBlocksToProcess(BlockReq, AverageBlockInstrs);
  }
}

void BlockGenPlanningImpl::fillReqWithPlainInstsByNumber(
    planning::FunctionRequest &FunReq, size_t NumInstrPlain,
    size_t AverageBlockInstrs) {
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &GenSettings = GenCtx->getGenSettings();
  while (NumInstrPlain > 0) {
    auto MaxBlockInstrs = AverageBlockInstrs * 2ull;
    auto InstrsToAdd = RandEngine::genInInterval(
        1ull, std::min<unsigned long long>(NumInstrPlain, MaxBlockInstrs));

    auto BlockId = RandEngine::genInRange(BlocksToProcess.size());
    const auto *MBB = BlocksToProcess[BlockId];

    auto BlockReqIt = FunReq.find(MBB);
    unsigned Limit = 0;
    if (BlockReqIt != FunReq.end()) {
      auto &BlockReq = BlockReqIt->second;
      assert(BlockReq.limit().isNumLimit());
      Limit = BlockReq.limit().getLimit();
    }
    InstrsToAdd = std::min(InstrsToAdd, MaxBlockInstrs - Limit);

    auto Lim = planning::RequestLimit::NumInstrs{InstrsToAdd};
    FunReq.addToBlock(MBB, planning::InstructionGroupRequest(
                               Lim, planning::createGenPolicy(
                                        ProgCtx, GenSettings, *MBB,
                                        FunReq.getOpcodeWeightOverrides())));
    NumInstrPlain -= InstrsToAdd;
    auto &BlockReq = FunReq.at(MBB);
    updateBlocksToProcess(BlockReq, AverageBlockInstrs);
  }
}

void BlockGenPlanningImpl::updateBlocksToProcess(
    const planning::BasicBlockRequest &BlockReq, size_t AverageBlockInstrs) {
  // FIXME: We should make a smarter choice allowing big BBs with a low
  // probability instead of allowing BB sizes only in [0, 2 * Average block
  // size].
  assert(BlockReq.limit().isNumLimit());
  if (BlockReq.limit().getLimit() >= AverageBlockInstrs * 2)
    erase(BlocksToProcess, &BlockReq.getMBB());
}

template <typename T>
void addEmptyReqForBlocks(planning::FunctionRequest &FunReq, const T &Blocks,
                          const planning::RequestLimit &Limit) {
  for (auto *MBB : Blocks)
    FunReq.add(MBB, planning::BasicBlockRequest(*MBB));
}

static void randomize(const planning::FunctionRequest &FunReq,
                      planning::BasicBlockRequest &BB, GeneratorContext &GC) {
  if (BB.empty())
    return;

  auto IsRegular = [](const auto &Req) {
    return !Req.isInseparableBundle(); // FIXME: some kind of enum
                                       // or llvm rtti
  };
  // We request all plain packs to be after burst packs.
  auto InstPackIt = find_if(BB, IsRegular);
  if (InstPackIt == BB.end())
    return;
  auto &ProgCtx = GC.getProgramContext();
  auto &GenSettings = GC.getGenSettings();

  assert(std::all_of(InstPackIt, BB.end(), IsRegular) &&
         "Before randomization all 'plain' packs must be at the end.");

  // Calculate the number of instructions in all plain packs. Since plain packs
  // may differ only by instruction count, we can think that only one plain pack
  // exists with instcount of RegularPackSize.
  auto RegularPackSize = std::accumulate(InstPackIt, BB.end(), 0ull,
                                         [](auto Acc, const auto &Pack) {
                                           auto &Limit = Pack.limit();
                                           assert(Limit.isNumLimit());
                                           return Limit.getLimit() + Acc;
                                         });

  // At this point let's say that we have `RegularPackSize` packs of plain
  // instructions each of size one, e.g.:
  //   P_0  P_1  P_2  P_3  P_4  P_5
  // Let's also say that we have N burst packs:
  //   B_0  B_1  B_2
  // We generate N indices in [0, RegularPackSize - 1] for plain packs. By these
  // indices plain packs will be mixed with burst packs:
  //   Idxs: 0, 1, 1, 4 (generate 3 idxs as there are three burst packs).
  //   Regular packs with idxs [0, 1) will be added before the first burst pack
  //   Regular packs with idxs [1, 1) (<--- no packs) will be added before the
  //   second burst pack and so on. If the last index is smaller than
  //   RegularPackSize than all plain packs left will be added after the last
  //   burst pack (in our case they are P_4 and P_5).
  // If we add more than one plain pack, then they'll be joint into one.
  // Resulting sequence of packs for our example is:
  //   { Regular, 1 } -- P_0
  //   { B_0 }
  //   { B_1 }
  //   { Regular, 3 } -- P_1  P_2  P_3
  //   { B_2 }
  //   { Regular, 2 } -- P_4  P_5
  std::vector<size_t> Idxs;
  Idxs.push_back(0);
  std::generate_n(std::back_inserter(Idxs),
                  std::distance(BB.begin(), InstPackIt), [RegularPackSize] {
                    return RandEngine::genInRange(0ull, RegularPackSize);
                  });
  std::sort(Idxs.begin(), Idxs.end());

  auto NonRegularPacksIt = BB.begin();
  auto &MBB = BB.getMBB();
  planning::BasicBlockRequest NewPacks(MBB);
  for (auto [FirstIdx, SecondIdx] : zip(Idxs, drop_begin(Idxs))) {
    assert(FirstIdx <= SecondIdx);
    if (FirstIdx != SecondIdx)
      NewPacks.add(planning::InstructionGroupRequest(
          planning::RequestLimit::NumInstrs{SecondIdx - FirstIdx},
          planning::createGenPolicy(ProgCtx, GenSettings, MBB,
                                    FunReq.getOpcodeWeightOverrides())));
    // FIXME: should be enum or llvm rtti
    assert(NonRegularPacksIt->isInseparableBundle());
    NewPacks.add(std::move(*NonRegularPacksIt));
    assert(NonRegularPacksIt != BB.end());
    ++NonRegularPacksIt;
  }
  if (Idxs.back() != RegularPackSize)
    NewPacks.add(planning::InstructionGroupRequest(
        planning::RequestLimit::NumInstrs{RegularPackSize - Idxs.back()},
        planning::createGenPolicy(ProgCtx, GenSettings, MBB,
                                  FunReq.getOpcodeWeightOverrides())));
  std::swap(NewPacks, BB);
}

template <typename Predicate>
void BlockGenPlanningImpl::fillBlocksToProcess(
    const MachineFunction &MF, planning::FunctionRequest &FunReq,
    Predicate &&Pred) {
  auto MapRange = map_range(MF, [](auto &MBB) { return &MBB; });
  auto DropBlock = [&MapRange, &FunReq] {
    FunReq.add(*MapRange.begin(),
               planning::BasicBlockRequest(**MapRange.begin()));
    MapRange = drop_begin(MapRange);
  };

  auto IsRegsInit = GenCtx->getGenSettings().RegistersConfig.InitializeRegs;
  if (IsRegsInit && FG->isEntryFunction(MF))
    DropBlock();
  copy_if(std::move(MapRange), std::back_inserter(BlocksToProcess),
          std::forward<Predicate>(Pred));
}

planning::FunctionRequest
BlockGenPlanningImpl::processFunctionWithNumInstr(const MachineFunction &MF) {
  assert(GenCtx->getGenSettings().getGenerationMode() ==
         GenerationMode::NumInstrs);

  auto LatchBlocks = collectLatchBlocks(*GenCtx, *MLI, MF, SimCtx);
  planning::FunctionRequest FunReq(MF, *GenCtx);
  fillBlocksToProcess(MF, FunReq, [&LatchBlocks](const auto *MBB) {
    return !LatchBlocks.count(MBB);
  });
  assert(!BlocksToProcess.empty() &&
         "At least one basic block that is not a latch block must exist");

  auto NumInstrTotal = FG->getRequestedInstrsNum(MF);
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &GenSettings = GenCtx->getGenSettings();
  const auto &OpCC = ProgCtx.getOpcodeCache();
  assert(NumInstrTotal >= GenSettings.getCFInstrsNum(OpCC, NumInstrTotal));
  NumInstrTotal -= GenSettings.getCFInstrsNum(OpCC, NumInstrTotal);

  const auto &Cfg = GenCtx->getConfig();
  // FIXME: NumInstrBurst should be somehow randomized. But we must be careful
  // as in some cases there are no instructions outside burst groups and then
  // the number must be exact.
  unsigned long long NumInstrBurst = getBurstProb(Cfg, *GenCtx) * NumInstrTotal;
  auto NumInstrPlain = NumInstrTotal;
  NumInstrPlain -= NumInstrBurst;

  auto AverageBlockInstrs = NumInstrTotal / BlocksToProcess.size();
  if (AverageBlockInstrs == 0)
    AverageBlockInstrs = 1;

  fillReqWithBurstGroups(FunReq, NumInstrBurst, NumInstrTotal,
                         AverageBlockInstrs);
  fillReqWithPlainInstsByNumber(FunReq, NumInstrPlain, AverageBlockInstrs);

  for (auto &[MBB, BB] : FunReq)
    erase(BlocksToProcess, MBB);
  // Add default plans for remaining blocks.
  addEmptyReqForBlocks(FunReq, BlocksToProcess,
                       planning::RequestLimit::NumInstrs{});
  addEmptyReqForBlocks(FunReq, LatchBlocks,
                       planning::RequestLimit::NumInstrs{});
  // Randomize generation plan: shuffle burst groups and plain instructions.
  for (auto &BlockReq : make_second_range(FunReq))
    randomize(FunReq, BlockReq, *GenCtx);

  return FunReq;
}

void BlockGenPlanningImpl::fillReqWithPlainInstsBySize(
    planning::FunctionRequest &FunReq, size_t MFSizeLimit) {
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &GenSettings = GenCtx->getGenSettings();
  auto MaxInstrSize =
      ProgCtx.getLLVMState().getSnippyTarget().getMaxInstrSize();
  // Multiset instead of vector because we generate accumulated size of first k
  // blocks. For example, if RequestedAccumulatedSizes == {2, 4, 4, 25, 37} then
  // blocks will generated with {2, 2, 0, 21, 12} sizes.
  std::multiset<size_t> RequestedAccumulatedSizes = {MFSizeLimit};
  transform(
      seq(0ul, BlocksToProcess.size() - 1),
      std::inserter(RequestedAccumulatedSizes, RequestedAccumulatedSizes.end()),
      [=](auto) {
        return RandEngine::genInInterval(MFSizeLimit) / MaxInstrSize *
               MaxInstrSize;
      });
  size_t LastAccumulatedSize = 0;
  for (auto [MBB, AccumulatedSize] :
       zip(BlocksToProcess, RequestedAccumulatedSizes)) {
    size_t BlockSize = AccumulatedSize - LastAccumulatedSize;
    auto Limit = planning::RequestLimit::Size{BlockSize};
    FunReq.addToBlock(MBB, planning::InstructionGroupRequest(
                               Limit, planning::createGenPolicy(
                                          ProgCtx, GenSettings, *MBB,
                                          FunReq.getOpcodeWeightOverrides())));
    LastAccumulatedSize = AccumulatedSize;
  }
}

planning::FunctionRequest
BlockGenPlanningImpl::processFunctionWithSize(const MachineFunction &MF) {
  assert(GenCtx->getGenSettings().getGenerationMode() == GenerationMode::Size);

  planning::FunctionRequest FunReq(MF, *GenCtx);
  fillBlocksToProcess(MF, FunReq, [](auto *MBB) { return true; });
  assert(!BlocksToProcess.empty() && "At least one basic block must exist");

  auto MFSizeLimit = calculateMFSizeLimit(MF);

  fillReqWithPlainInstsBySize(FunReq, MFSizeLimit);

  return FunReq;
}

static size_t calcFilledSize(const planning::FunctionRequest &FunReq,
                             ArrayRef<const MachineBasicBlock *> Blocks,
                             const SnippyTarget &SnpTgt, LLVMState &State) {
  size_t FilledSize = 0;
  for (auto *Block : Blocks) {
    FilledSize += State.getMBBSize(*Block);
    if (FunReq.count(Block)) {
      auto &Limit = FunReq.at(Block).limit();
      assert(Limit.isSizeLimit());
      FilledSize += Limit.getLimit();
    }
  }
  return FilledSize;
}

static void setSizeForLoopBlock(planning::FunctionRequest &FunReq,
                                const MachineBasicBlock &SelectedMBB,
                                ArrayRef<const MachineBasicBlock *> LoopBlocks,
                                NumericRange<ProgramCounterType> PCDist,
                                bool IsLatch, GeneratorContext &SGCtx) {
  assert(!FunReq.count(&SelectedMBB));
  auto &ProgCtx = SGCtx.getProgramContext();
  const auto &GenSettings = SGCtx.getGenSettings();
  auto &State = ProgCtx.getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto BrOpc = SelectedMBB.getFirstTerminator()->getOpcode();
  auto MaxBranchDstMod = SnpTgt.getMaxBranchDstMod(BrOpc);
  if (PCDist.Max.has_value() && PCDist.Max.value() > MaxBranchDstMod) {
    auto OpName = SGCtx.getProgramContext().getOpcodeCache().name(BrOpc);
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

  size_t FilledSize = calcFilledSize(FunReq, LoopBlocks, SnpTgt, State);
  if (IsLatch) { // Branches size isn't included in backward distance
    auto BranchesSize = State.getCodeBlockSize(SelectedMBB.getFirstTerminator(),
                                               SelectedMBB.end());
    assert(BranchesSize <= FilledSize);
    FilledSize -= BranchesSize;
  }

  size_t MBBSize = State.getMBBSize(SelectedMBB);
  size_t NumOfPrimaryInstrs =
      countPrimaryInstructions(SelectedMBB.begin(), SelectedMBB.end());

  if (PCDist.Max.value() < FilledSize) {
    std::string Desc = formatv("Loop is already filled with {0}"
                               " bytes, but max pc distance is {1}.",
                               FilledSize, PCDist.Max.value());
    const auto &Branches = SGCtx.getGenSettings().Cfg.Branches;
    if (Branches.isRandomCountersInitRequested() &&
        Branches.isPCDistanceRequested())
      Desc +=
          " This can be caused by small PC distance with random loop counter "
          "initialization, you can try either relax PC distance requirements "
          "or reduce loop counter initialization to values close to 0.";
    snippy::fatal(SelectedMBB.getParent()->getFunction().getContext(),
                  "Max PC distance requirement can't be met", Desc);
  }

  NumericRange<unsigned> BlockRange;
  BlockRange.Max = PCDist.Max.value() - FilledSize;
  if (PCDist.Min.has_value())
    BlockRange.Min =
        (PCDist.Min.value() > FilledSize) ? PCDist.Min.value() - FilledSize : 0;

  auto InstrsSizes = SnpTgt.getPossibleInstrsSize(
      State.getSubtargetImpl(SelectedMBB.getParent()->getFunction()));
  assert(InstrsSizes.size() > 0 &&
         "Target must have at least one variant of instruction size");
  auto MinInstrSize = *InstrsSizes.begin();
  auto Min = alignTo(BlockRange.Min.value_or(0), MinInstrSize);
  auto Max = alignDown(BlockRange.Max.value(), MinInstrSize);
  LLVM_DEBUG(dbgs() << "Selected MBB: "; SelectedMBB.dump());
  LLVM_DEBUG(dbgs() << "BlockRange.Min == " << BlockRange.Min << "\n");
  LLVM_DEBUG(dbgs() << "BlockRange.Max == " << BlockRange.Max << "\n");
  LLVM_DEBUG(dbgs() << "MinInstrSize == " << MinInstrSize << "\n");
  LLVM_DEBUG(dbgs() << "Min == " << Min << "\n");
  LLVM_DEBUG(dbgs() << "Max == " << Max << "\n");
  if (Min > Max)
    snippy::fatal(SelectedMBB.getParent()->getFunction().getContext(),
                  "Max PC distance requirement can't be met",
                  "Min distance is " + Twine(Min) + " , but max distance is " +
                      Twine(Max));

  auto BlockSize = RandEngine::genInInterval(Min, Max);
  BlockSize = alignDown(BlockSize, MinInstrSize);
  auto Limit = planning::RequestLimit::Size{BlockSize};
  // InitialAmount allows to account for any already generated instructions
  auto InitialAmount =
      GenerationStatistics{NumOfPrimaryInstrs, /*GeneratedSize*/ MBBSize};
  FunReq.addToBlock(&SelectedMBB, planning::InstructionGroupRequest(
                                      Limit,
                                      planning::createGenPolicy(
                                          ProgCtx, GenSettings, SelectedMBB,
                                          FunReq.getOpcodeWeightOverrides()),
                                      InitialAmount));
}

void BlockGenPlanningImpl::fillReqForTopLoopBySize(
    planning::FunctionRequest &FunReq, const MachineLoop &ML) const {
  assert(ML.isOutermost() && "Only top level loop expected");
  auto &ProgCtx = GenCtx->getProgramContext();
  if (!ML.getSubLoops().empty())
    fatal(ProgCtx.getLLVMState().getCtx(), "Block generation planning failed",
          "PC distance is now supported with max loop depth 1");

  auto PCDist = GenCtx->getConfig().Branches.getPCDistance();

  auto LoopBlocks = ML.getBlocks();
  for (auto *MBB : LoopBlocks)
    setSizeForLoopBlock(FunReq, *MBB, LoopBlocks, PCDist, ML.isLoopLatch(MBB),
                        *GenCtx);
}

planning::FunctionRequest
BlockGenPlanningImpl::processFunctionMixed(const MachineFunction &MF) {
  const auto &GenSettings = GenCtx->getGenSettings();
  assert(GenSettings.getGenerationMode() == GenerationMode::Mixed);

  planning::FunctionRequest FunReq(MF, *GenCtx);
  // Process blocks out of loops
  fillBlocksToProcess(
      MF, FunReq, [this](const auto *MBB) { return !MLI->getLoopFor(MBB); });
  unsigned SupposedNumInstr = 0;
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &OpCC = ProgCtx.getOpcodeCache();
  auto MaxInstrSize =
      ProgCtx.getLLVMState().getSnippyTarget().getMaxInstrSize();
  for (auto *ML : *MLI) {
    assert(ML);
    fillReqForTopLoopBySize(FunReq, *ML);
    for (auto *MBB : ML->blocks()) {
      auto &Limit = FunReq.at(MBB).limit();
      assert(Limit.isSizeLimit());
      auto BBSize = Limit.getLimit();
      SupposedNumInstr += BBSize / MaxInstrSize;
      SupposedNumInstr += BBSize % MaxInstrSize ? 1 : 0;
    }
  }

  auto NumInstrTotal = FG->getRequestedInstrsNum(MF);
  assert(NumInstrTotal >= GenSettings.getCFInstrsNum(OpCC, NumInstrTotal));
  NumInstrTotal -= GenSettings.getCFInstrsNum(OpCC, NumInstrTotal);
  // If number of instructions in size-requested blocks is already enough for
  // the whole function, skipping num instrs planning for other blocks
  if (NumInstrTotal <= SupposedNumInstr) {
    addEmptyReqForBlocks(FunReq, BlocksToProcess,
                         planning::RequestLimit::NumInstrs{});
    return FunReq;
  }

  NumInstrTotal -= SupposedNumInstr;

  const auto &Cfg = GenCtx->getConfig();
  // FIXME: NumInstrBurst should be somehow randomized. But we must be careful
  // as in some cases there are no instructions outside burst groups and then
  // the number must be exact.
  unsigned long long NumInstrBurst = getBurstProb(Cfg, *GenCtx) * NumInstrTotal;

  auto NumInstrPlain = NumInstrTotal;
  NumInstrPlain -= NumInstrBurst;

  auto AverageBlockInstrs = NumInstrTotal / BlocksToProcess.size();
  if (AverageBlockInstrs == 0)
    AverageBlockInstrs = 1;

  fillReqWithBurstGroups(FunReq, NumInstrBurst, NumInstrTotal,
                         AverageBlockInstrs);

  fillReqWithPlainInstsByNumber(FunReq, NumInstrPlain, AverageBlockInstrs);
  for (auto &[MBB, BB] : FunReq)
    erase(BlocksToProcess, MBB);
  // Add default plans for remaining blocks.
  addEmptyReqForBlocks(FunReq, BlocksToProcess,
                       planning::RequestLimit::NumInstrs{});

  return FunReq;
}

static void checkGenModeCompatibility(GeneratorContext &GenCtx,
                                      const MachineLoopInfo &MLI,
                                      SimulatorContext &SimCtx) {
  auto GM = GenCtx.getGenSettings().getGenerationMode();
  if (GM == GenerationMode::NumInstrs)
    return;

  bool LoopGenerated = !MLI.empty();
  bool TrackingEnabled = SimCtx.hasTrackingMode();
  if (LoopGenerated && TrackingEnabled)
    snippy::fatal(
        "Generation by size with loops in tracking mode is not supported");
}

planning::FunctionRequest
BlockGenPlanningImpl::processFunction(const MachineFunction &MF) {
  assert(GenCtx && MLI && FG);
  checkGenModeCompatibility(*GenCtx, *MLI, SimCtx);
  switch (GenCtx->getGenSettings().getGenerationMode()) {
  case GenerationMode::NumInstrs:
    return processFunctionWithNumInstr(MF);
  case GenerationMode::Size:
    return processFunctionWithSize(MF);
  case GenerationMode::Mixed:
    return processFunctionMixed(MF);
  }
  llvm_unreachable("unknown generation mode");
}

const planning::BasicBlockRequest &
BlockGenPlanning::get(const MachineBasicBlock &MBB) const {
  assert(Req);
  assert(Req->count(&MBB));
  return Req->at(&MBB);
}

bool BlockGenPlanning::runOnMachineFunction(MachineFunction &MF) {
  auto *GenCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  auto *MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  auto *GenPlanWrapper = &getAnalysis<BlockGenPlanWrapper>();
  auto *FG = &getAnalysis<FunctionGenerator>();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();

  BlockGenPlanningImpl Impl(GenCtx, MLI, FG, SimCtx);
  Req = Impl.processFunction(MF);
  assert(Req.has_value());
  GenPlanWrapper->setFunctionRequest(&MF, Req.value());

  return true;
}

void BlockGenPlanning::releaseMemory() { Req->clear(); }

} // namespace snippy
} // namespace llvm
