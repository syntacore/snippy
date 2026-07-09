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
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SMCManager.h"
#include "snippy/GeneratorUtils/LLVMState.h"
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

using namespace planning;

/// \class FunctionRequestWrapper
/// \brief The class wraps FunctionRequest to track which basic blocks still
/// need to be filled in (UnfilledBlocks). It redirects calls to FunctionRequest
/// methods through `callWithUpdate` and `call`.
/// After each basic block update via method `callWithUpdate`, the array of
/// unfilled blocks is updated. It is recommended to use this method for all
/// FunctionRequest modifications.
/// If FunctionRequest method should not be accompanied by an update of blocks,
/// then you need to use method `call`.
class FunctionRequestWrapper {
  planning::FunctionRequest &FunReq;
  std::optional<size_t> AverageBlockLimit;
  // Basic blocks which still have place to fill in with instructions. When the
  // block reaches the limit (instructions or size became bigger than
  // 2 * AverageBlockLimit), we stop filling it and remove it from this vector.
  std::vector<const MachineBasicBlock *> UnfilledBlocks;

public:
  FunctionRequestWrapper(planning::FunctionRequest &FunReq) : FunReq(FunReq) {}
  template <typename Predicate>
  void initUnfilledBlocks(GeneratorContext *GenCtx, const FunctionGenerator *FG,
                          const MachineFunction &MF, Predicate &&Pred);
  void setAverageBlockLimit(size_t NumInstrsLeft);
  const std::vector<const MachineBasicBlock *> &unfilledBlocks() const {
    return UnfilledBlocks;
  };
  SmallVector<size_t> getNumCtxGroupsPerMBBs() const {
    return FunReq.getNumCtxGroupsPerMBBs(UnfilledBlocks);
  }
  void fillEmptyBlocks();
  void dump() const;

  template <typename FuncT, typename... ArgsT>
  auto call(FuncT &&F, ArgsT &&...Args)
      -> std::invoke_result_t<FuncT, decltype(FunReq), ArgsT...> {
    return std::invoke(std::forward<FuncT>(F), FunReq,
                       std::forward<ArgsT>(Args)...);
  }

  template <typename FuncT, typename... ArgsT>
  void callWithUpdate(FuncT &&F, const MachineBasicBlock *MBB,
                      ArgsT &&...Args) {
    std::invoke(std::forward<FuncT>(F), FunReq, MBB,
                std::forward<ArgsT>(Args)...);
    updateUnfilledBlocks(MBB);
  }

private:
  void updateUnfilledBlocks(const MachineBasicBlock *MBB);
};

class BlockGenPlanningImpl {
  GeneratorContext *GenCtx;
  const MachineLoopInfo *MLI;
  const FunctionGenerator *FG;
  SimulatorContext SimCtx;
  FunctionRequestWrapper FunBlocks;

public:
  BlockGenPlanningImpl(GeneratorContext *GenCtxIn, const MachineLoopInfo *MLIIn,
                       const FunctionGenerator *FGIn, SimulatorContext SimCtx,
                       planning::FunctionRequest &FunReq)
      : GenCtx(GenCtxIn), MLI(MLIIn), FG(FGIn), SimCtx(std::move(SimCtx)),
        FunBlocks(FunReq) {}

  void processFunction(const MachineFunction &MF);

private:
  void processFunctionWithNumInstr(const MachineFunction &MF);
  void processFunctionWithSize(const MachineFunction &MF);
  void processFunctionMixed(const MachineFunction &MF);

  const std::vector<const MachineBasicBlock *> &unfilledBlocks() const {
    return FunBlocks.unfilledBlocks();
  };
  size_t calculateMFSizeLimit(const MachineFunction &MF) const;

  auto findSuitableBBAndContextGroup(
      const BurstGramData::UniqueOpcodesTy &BurstGroup);

  size_t fillReqWithBurstGroups(size_t NumInstrsLeft, size_t NumInstrTotal);

  void addNonVectorBurstGroup(size_t BurstGroupInstCount, size_t GroupId);
  void fillReqWithSMC(const Function &SMCCopyFunc, const Function &SMCTgtFunc);

  size_t fillReqWithContextModeChanges(size_t NumInstrPlain);

  template <typename RequestLimitType>
  void fillReqWithPlainInsts(size_t PlainLimit, size_t Alignment);

  void fillReqForTopLoopBySize(const MachineLoop &ML);

  void splitDefaultGroup(planning::BasicBlockRequest &BBReq, size_t Alignment);
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
  assert(
      !GenCtx->getConfig().PassCfg.InstrsGenerationConfig.isInstrsNumKnown());
  auto OutSectionDesc = GenCtx->getProgramContext().getOutputSectionFor(MF);
  auto MaxSize = OutSectionDesc.Size;
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnpTgt = State.getSnippyTarget();
  auto CurrentCodeSize = State.getFunctionSize(MF);
  const auto &Cfg = GenCtx->getConfig();
  const auto &ProgCfg = Cfg.ProgramCfg;
  auto &PassCfg = Cfg.PassCfg;
  auto MemMode = ProgCfg.MemoryCfg.InitializationMode;
  if (MemMode.Value == MemInitMode::RuntimeFull) {
    // actual size of __snippy_random may vary due to compressed instructions
    CurrentCodeSize += SnpTgt.getRandomGenFunctionMaxSize();
  } else if (MemMode.Value == MemInitMode::LoadsOnly ||
             MemMode.Value == MemInitMode::LoadsWithAddresses) {
    snippy::fatal("Incompatible options",
                  "Runtime memory initialization using loads and "
                  "num-instrs=all are incompatible");
  }
  // last instruction in the trace might be target dependent: EBREAK or
  // int 3, etc.
  StringRef LastInstr = PassCfg.InstrsGenerationConfig.LastInstr;
  // If not entry function, we generate ret anyway.
  bool EmptyLastInstr = FG->isEntryFunction(MF) && LastInstr.empty();
  auto SizeOfOpc = SnpTgt.getMaxInstrSize();

  // FIXME: lastInstructions == we reserve space to put final instruction
  // and any additional instructions that will be placed after random
  // instructions generation. This should be replaced as we have BlockInfo
  auto RegsSpilledToStack = ProgCfg.getRegsSpilledToStack();
  auto RegsSpilledToMem = ProgCfg.getRegsSpilledToMem();
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

static double getBurstProbWithoutCF(const Config &Cfg,
                                    GeneratorContext &GenCtx) {
  static constexpr auto TotalProb = 1.0;

  auto CFProb = Cfg.Histogram.getCFProbability(
      GenCtx.getProgramContext().getOpcodeCache());
  auto BurstProb = Cfg.getBurstOpcodesProbability();
  BurstProb =
      isZero(TotalProb - CFProb) ? 0.0 : BurstProb / (TotalProb - CFProb);
  return BurstProb;
}

static size_t getBurstNumInstr(GeneratorContext &GenCtx,
                               unsigned NumInstrsLeft) {
  const auto &Cfg = GenCtx.getConfig();
  static constexpr auto TotalProb = 1.0;

  auto BurstProb = getBurstProbWithoutCF(Cfg, GenCtx);
  // Due to FP errors, BurstProb can be slightly greater than 1.0,
  // so we need to limit it, otherwise the result will be > NumInstrsLeft.
  BurstProb = std::min(BurstProb, TotalProb);
  return std::ceil(BurstProb * NumInstrsLeft);
}

// Returns number of instructions that each burst group must have in the
// resulting snippet (mapping from instr count to burst group id). Number of
// instruction for each burst group is calculated in accordance with opcode
// weights from it. Multimap because different burst groups might have the same
// num instrs.
using NumInstrToGroupIdTy = std::multimap<size_t, size_t>;
static NumInstrToGroupIdTy getBurstInstCounts(GeneratorContext &GenCtx,
                                              size_t NumInstrBurst,
                                              size_t NumInstrTotal) {
  const auto &Cfg = GenCtx.getConfig();
  if (!Cfg.BurstConfig)
    return {};
  const auto &BGram = Cfg.BurstConfig->Burst;
  if (!BGram.Groupings)
    return {};
  assert(BGram.Groupings->size() > 0);

  auto OpcodeToNumOfGroups = BGram.getOpcodeToNumBurstGroups();
  NumInstrToGroupIdTy NumInstrToGroupId;
  auto InstrLeft = NumInstrBurst;
  for (const auto &[Idx, Group] : enumerate(drop_end(*BGram.Groupings))) {
    auto Probability =
        std::accumulate(Group.begin(), Group.end(), 0.0,
                        [&OpcodeToNumOfGroups, &Cfg](double Acc, auto Opcode) {
                          assert(OpcodeToNumOfGroups.count(Opcode));
                          // If an opcode is used more in one burst group, its
                          // probability must be distributed among these groups.
                          return Acc + Cfg.Histogram.probability(Opcode) /
                                           OpcodeToNumOfGroups[Opcode];
                        });

    unsigned long long GroupNumInstrTotal = Probability * NumInstrTotal;
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

static auto getRandomIndices(size_t Size) {
  auto Indices = RandEngine::genNUniqInInterval(0ul, Size - 1, Size);
  assert(Indices);
  return *Indices;
}

// Randomly distribute burst groups over generation plan for BBs from
// UnfilledBlocks.
//
// Short algo example:
//
// NumInstrBurst is 21, burst group size is 7, AverageBlockLimit is 5, five BBs
// to fill.
//   NumInstrToGroupId    UnfilledBlocks    FunReq
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
// has the biggest num instrs and any random BB (e.g. BB3) from UnfilledBlocks.
// Then we add one burst group of size 7 with id1 to generation plan for the
// BB. Next step is to update NumInstrToGroupId map: `1: 8 -> id1` -> `1: 1 ->
// id1` as seven instructions were already added to plan.
//
// After the first iteration:
//   NumInstrToGroupId    UnfilledBlocks    FunReq
//     1: 1 -> id1          BB1 -> 0         BB1 -> empty
//     2: 5 -> id2          BB2 -> 0         BB2 -> empty
//     3: 4 -> id3          BB3 -> 7         BB3 -> Burst[7, id1]
//     4: 4 -> id4          BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// Next iteration: we take group 2 as it has the biggest inst count to plan and
// random BB (e.g. BB2). As you can see, group `2: 5 -> id2` doesn't have 7
// instructions, so we'll take five instructions from it and steal additional
// two from other groups. Current algorithm implementation steals instructions
// from groups with the smallest number of instructions to plan. In our case
// they are group 1 and group 3 (or 4, but let's use 3).
//
// After the iteration:
//   NumInstrToGroupId    UnfilledBlocks    FunReq
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
//   NumInstrToGroupId    UnfilledBlocks    FunReq
//    ~1: 0 -> id1~         BB1 -> 0         BB1 -> empty
//    ~2: 0 -> id2~        ~BB2 -> 14~       BB2 -> Burst[7, id2], Burst[7, id4]
//    ~3: 0 -> id3~         BB3 -> 7         BB3 -> Burst[7, id1]
//    ~4: 0 -> id4~         BB4 -> 0         BB4 -> empty
//                          BB5 -> 0         BB5 -> empty
//
// NB: after the last iteration we crossed out BB2 from UnfilledBlocks as it
// became bigger than 2 * AverageBlockLimit. So, no more packs would be added
// to it if we continued. The rule that excludes blocks must be improved, but
// currently it preserves the old behavior.

// We select a single context group with vector configuration compatible with
// the largest number of opcodes from the group and insert the burst group in
// this single context group.
auto BlockGenPlanningImpl::findSuitableBBAndContextGroup(
    const BurstGramData::UniqueOpcodesTy &BurstGroup) {
  assert(!BurstGroup.empty());
  auto MaxSuitableOpcodes = 0u;
  std::optional<std::pair<const MachineBasicBlock *,
                          planning::BasicBlockRequest::iterator>>
      ResultOpt;
  // This is necessary in order to find different places to insert each time
  // and evenly fill all basic blocks with burst groups.
  const auto &Blocks = unfilledBlocks();
  auto BBIndices = getRandomIndices(Blocks.size());

  for (auto BBIdx : BBIndices) {
    const auto *MBB = Blocks[BBIdx];
    auto &BBReq = FunBlocks.call(&FunctionRequest::get, MBB);
    auto Indices = getRandomIndices(BBReq.size());
    for (auto Idx : Indices) {
      const auto &SingleGroup = BBReq[Idx];
      assert(SingleGroup.getOpcodeFilter().has_value());
      auto SuitableOpcodes =
          count_if(BurstGroup, *SingleGroup.getOpcodeFilter());
      if (SuitableOpcodes <= MaxSuitableOpcodes)
        continue;
      MaxSuitableOpcodes = SuitableOpcodes;
      ResultOpt = std::make_pair(MBB, BBReq.begin() + Idx);
      if (MaxSuitableOpcodes == BurstGroup.size())
        return *ResultOpt;
    }
  }
  if (MaxSuitableOpcodes == 0u)
    snippy::fatal(
        "Can't find suitable RVV configuration for burst group. Please make "
        "sure that at least one opcode from each burst group has compatible "
        "RVV configuration. You can increase the configuration space in "
        "riscv-vector-unit or remove incompatible burst groups.");

  assert(ResultOpt.has_value());
  return *ResultOpt;
}

void BlockGenPlanningImpl::addNonVectorBurstGroup(size_t BurstGroupInstCount,
                                                  size_t GroupId) {
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &Cfg = GenCtx->getConfig();
  assert(Cfg.BurstConfig);
  auto *RandomBB = RandEngine::selectFromContainer(unfilledBlocks());
  auto &BBReq = FunBlocks.call(&FunctionRequest::get, RandomBB);
  auto RandomSG = RandEngine::selectItFromContainer(BBReq);
  FunBlocks.callWithUpdate(
      &FunctionRequest::addToBlockIn, RandomBB, RandomSG,
      planning::InstructionGroupRequest(
          planning::RequestLimit::NumInstrs{BurstGroupInstCount},
          planning::BurstGenPolicy(ProgCtx, *Cfg.BurstConfig, GroupId)));
}

size_t BlockGenPlanningImpl::fillReqWithBurstGroups(size_t NumInstrsLeft,
                                                    size_t NumInstrTotal) {
  // FIXME: NumInstrBurst should be somehow randomized. But we must be careful
  // as in some cases there are no instructions outside burst groups and then
  // the number must be exact.
  auto NumInstrBurst = getBurstNumInstr(*GenCtx, NumInstrsLeft);
  assert(NumInstrBurst <= NumInstrsLeft &&
         "It's impossible to generate more instructions than there is left in "
         "request");
  if (NumInstrBurst == 0)
    return 0;
  auto NumInstrBurstLeft = NumInstrBurst;
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto NumInstrToGroupId =
      getBurstInstCounts(*GenCtx, NumInstrBurstLeft, NumInstrTotal);
  auto &Cfg = GenCtx->getConfig();
  assert(Cfg.BurstConfig);
  const auto &BurstSettings = Cfg.BurstConfig->Burst;

  while (NumInstrBurstLeft > 0) {
    auto BurstGroupInstCount = RandEngine::genInRangeInclusive(
        BurstSettings.MinSize, BurstSettings.MaxSize);
    // The last burst group might be smaller than the minimum size requested in
    // the configuration. This matches the behavior we had before. The
    // difference is that this group can be placed in any random basic block,
    // not in the last block in the function as it was in the previous
    // implementation.
    BurstGroupInstCount =
        std::min<unsigned long long>(BurstGroupInstCount, NumInstrBurstLeft);

    auto GroupId = extractBurstGroup(NumInstrToGroupId, BurstGroupInstCount);

    const auto &Groupings = BurstSettings.Groupings.value();
    assert(GroupId < Groupings.size());
    const auto &Group = Groupings[GroupId];
    auto &InstrInfo = State.getInstrInfo();

    // This means that there is no RVV and we choose any block.
    if (none_of(Group, [&SnippyTgt, &InstrInfo](unsigned Opcode) {
          return SnippyTgt.isVectorInstr(InstrInfo.get(Opcode));
        })) {
      addNonVectorBurstGroup(BurstGroupInstCount, GroupId);
      NumInstrBurstLeft -= BurstGroupInstCount;
      continue;
    }
    auto [MBB, SGIt] = findSuitableBBAndContextGroup(Group);
    auto Filter = SGIt->getOpcodeFilter();
    assert(Filter.has_value());

    for_each(Group, [&InstrInfo, Filter = *Filter](auto Opcode) {
      // TODO: We need add info about selected configuration here, using
      // SGIt->createModeChangeIG().policy().print()
      if (!Filter(Opcode))
        snippy::warn(WarningName::BurstMode,
                     Twine("Opcode ") + InstrInfo.getName(Opcode) +
                         " will not be generated in the burst group "
                         "because it is incompatible with RVV configuration "
                         "selected for this group ",
                     "Please keep only valid configurations for all opcodes, "
                     "or change the group so that all "
                     "opcodes have a valid configuration.");
    });

    auto BurstPolicy =
        planning::BurstGenPolicy(ProgCtx, *Cfg.BurstConfig, GroupId, Filter);
    FunBlocks.callWithUpdate(
        &FunctionRequest::addToBlockIn, MBB, SGIt,
        planning::InstructionGroupRequest(
            planning::RequestLimit::NumInstrs{BurstGroupInstCount},
            std::move(BurstPolicy)));
    NumInstrBurstLeft -= BurstGroupInstCount;
  }
  return NumInstrBurst;
}

void BlockGenPlanningImpl::fillReqWithSMC(const Function &SMCCopyFunc,
                                          const Function &SMCTgtFunc) {
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &Config = GenCtx->getConfig();
  auto SMCTgtRatio = Config.PassCfg.SMC->SMCTgtBlocksRatio;
  const auto Blocks = unfilledBlocks();
  unsigned MBBNumToInsert = Blocks.size() * SMCTgtRatio;

  auto ToInsertRange =
      RandEngine::genNUniqInInterval(0ul, Blocks.size() - 1, MBBNumToInsert);
  if (!ToInsertRange)
    snippy::fatal("Error in getting random overwritting basic blocks for smc");
  for (auto BlockId : *ToInsertRange) {
    const auto *MBB = Blocks[BlockId];

    auto OverwritersNum = RandEngine::genInRangeInclusive(
        *Config.PassCfg.SMC->SMCOverwriters.Min,
        *Config.PassCfg.SMC->SMCOverwriters.Max);

    FunBlocks.callWithUpdate(
        &FunctionRequest::addToBlock<InstructionGroupRequest>, MBB,
        planning::InstructionGroupRequest(
            planning::RequestLimit::NumInstrs{/* WHAT? */ 0},
            planning::SMCGenPolicy(ProgCtx, *GenCtx, SMCCopyFunc, SMCTgtFunc,
                                   OverwritersNum)));
    --MBBNumToInsert;
  }
}

// Returns the number of primary mode-changing instructions added to
// this function request
size_t BlockGenPlanningImpl::fillReqWithContextModeChanges(size_t NumLimit) {
  auto &ProgCtx = GenCtx->getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  const auto Blocks = unfilledBlocks();
  if (!SnippyTgt.needToGenerateModeSwitches(ProgCtx)) {
    // Add to each BB only one empty context group to fill in
    for (const auto &MBB : Blocks)
      FunBlocks.callWithUpdate(&FunctionRequest::addToBlock<SingleContextGroup>,
                               MBB, planning::SingleContextGroup());
    return 0;
  }

  double PolicySwitchInstrsProbability =
      SnippyTgt.getModeSwitchProbability(ProgCtx);
  size_t ContextModeChangesAmount =
      std::ceil(NumLimit * PolicySwitchInstrsProbability);
  assert(ContextModeChangesAmount <= NumLimit);

  // At this stage, no block is filled yet (because
  // fillReqWithContextModeChanges is always called first), so UnfilledBlocks
  // contains all the blocks that need to be filled.
  auto BlocksAmount = Blocks.size();
  // If we have enough mode changes overall, we would prefer to
  // not have any support ones
  size_t MinimumChangesPerMBB =
      ContextModeChangesAmount >= BlocksAmount ? 1 : 0;
  // Distribute the mode changes between the blocks
  SmallVector<size_t> ModeChangesPerMBB;
  ModeChangesPerMBB.reserve(BlocksAmount);
  assert(BlocksAmount);
  RandEngine::splitNIntoMParts(ModeChangesPerMBB, ContextModeChangesAmount,
                               BlocksAmount, MinimumChangesPerMBB);

  SmallVector<bool> ModeChangeIsSupport(
      /*Size=*/BlocksAmount,
      /*Value=*/SnippyTgt.modeSwitchIsSupport(ProgCtx));

  for (auto &&[SupportMarker, ModeChanges] :
       zip_equal(ModeChangeIsSupport, ModeChangesPerMBB))
    // If we have no mode changes in a block, add a support one
    if (ModeChanges == 0) {
      SupportMarker = true;
      ModeChanges = 1;
    }

  auto GetSupportMetadataIfNeed = [&State](bool IsSupport) {
    return IsSupport ? getMetadataMark(State.getCtx(), SnippyMetadata::Support)
                     : nullptr;
  };
  for (const auto &[MBB, ModeChanges, IsSupport] :
       zip_equal(Blocks, ModeChangesPerMBB, ModeChangeIsSupport)) {
    assert(ModeChanges > 0);
    for (size_t I = 0; I < ModeChanges; ++I) {
      FunBlocks.callWithUpdate(
          &FunctionRequest::addToBlock<SingleContextGroup>, MBB,
          planning::SingleContextGroup(planning::ModeChangingInstPolicy(
              ProgCtx, *MBB, GetSupportMetadataIfNeed(IsSupport))));
    }
  }

  return SnippyTgt.modeSwitchIsSupport(ProgCtx) ? 0 : ContextModeChangesAmount;
}

static size_t getMinInstrSize(const MachineBasicBlock &MBB,
                              const SnippyTarget &Tgt) {
  const auto &STInfo = MBB.getParent()->getSubtarget();
  const auto &InstrsSizes = Tgt.getPossibleInstrsSize(STInfo);
  assert(InstrsSizes.size() > 0 &&
         "Target must have at least one variant of instruction size");
  assert(*InstrsSizes.begin() == *min_element(InstrsSizes));
  return *InstrsSizes.begin();
}

template <typename RequestLimitType>
void BlockGenPlanningImpl::fillReqWithPlainInsts(size_t PlainLimit,
                                                 size_t Alignment) {
  if (PlainLimit == 0) {
    FunBlocks.fillEmptyBlocks();
    return;
  }
  const auto &Cfg = GenCtx->getConfig();
  auto &ProgCtx = GenCtx->getProgramContext();

  // The more instruction groups are in a block, the more plain instructions
  // we want to be in that block.
  const auto &IGsPerMBB = FunBlocks.getNumCtxGroupsPerMBBs();
  assert(IGsPerMBB.size());
  SmallVector<size_t> PlainPerMBB;
  PlainPerMBB.reserve(IGsPerMBB.size());
  RandEngine::splitNIntoMPartsWeighted(PlainPerMBB, PlainLimit, IGsPerMBB,
                                       /*Baseline=*/0ul,
                                       /*Uniformity=*/0.0, Alignment);
  const auto Blocks = unfilledBlocks();
  assert(Blocks.size() == PlainPerMBB.size());
  for (auto &&[MBB, Plain] : zip_equal(Blocks, PlainPerMBB)) {
    // Add empty context only for default group
    FunBlocks.callWithUpdate(&FunctionRequest::addToBlock<SingleContextGroup>,
                             MBB, planning::SingleContextGroup());
    FunBlocks.callWithUpdate(
        &FunctionRequest::addToBlock<InstructionGroupRequest>, MBB,
        planning::InstructionGroupRequest(
            RequestLimitType{Plain},
            planning::createGenPolicy(ProgCtx, Cfg.DefFlowConfig)));
  }
  for (const auto &MBB : Blocks)
    splitDefaultGroup(FunBlocks.call(&FunctionRequest::get, MBB), Alignment);
  // Randomize generation plan: We can shuffle instructions only within the same
  // single context group, but not between different context groups.
  FunBlocks.call(&FunctionRequest::shuffle);
}

void FunctionRequestWrapper::updateUnfilledBlocks(
    const MachineBasicBlock *MBB) {
  // FIXME: We should make a smarter choice allowing big BBs with a low
  // probability instead of allowing BB sizes only in [0, 2 * Average block
  // size].
  auto &BlockReq = FunReq.get(MBB);
  assert(!BlockReq.limit().isMixedLimit());
  assert(AverageBlockLimit.has_value());
  if (BlockReq.limit().getLimit() >= *AverageBlockLimit * 2)
    erase(UnfilledBlocks, BlockReq.getMBB());
}

void FunctionRequestWrapper::setAverageBlockLimit(size_t SpaceLeft) {
  assert(!UnfilledBlocks.empty());
  AverageBlockLimit = SpaceLeft / UnfilledBlocks.size();
  if (AverageBlockLimit == 0)
    AverageBlockLimit = 1;
}

void FunctionRequestWrapper::fillEmptyBlocks() {
  for (auto *MBB : UnfilledBlocks) {
    if (FunReq.contains(MBB))
      continue;
    FunReq.add(MBB, planning::BasicBlockRequest(MBB));
  }
}

void FunctionRequestWrapper::dump() const {
  errs() << "FunctionRequestWrapper:\n";
  FunReq.print(errs());
  errs() << "\n";
}

void BlockGenPlanningImpl::splitDefaultGroup(planning::BasicBlockRequest &BBReq,
                                             size_t Alignment) {
  // We don't need to split default group into parts if there is only one.
  if (BBReq.numIGs() <= 1)
    return;
  const auto &Cfg = GenCtx->getConfig();
  auto &ProgCtx = GenCtx->getProgramContext();
  // We expect that policies are going in this order:
  // 1. zero or more other policies
  // 2. exactly one DefaultPolicy or ValuegramPolicy
  const auto &DefaultGroupLimit = BBReq.back().limit();
  assert(!DefaultGroupLimit.isMixedLimit());
  // Either size limit or num limit
  size_t NumericalLimit = DefaultGroupLimit.getLimit();

  // We need to split the default group into several parts and insert each one
  // of them between two non-default groups
  SmallVector<size_t> PlainGroupsSizes;
  RandEngine::splitNIntoMParts(PlainGroupsSizes, /* N */ NumericalLimit,
                               /* M (without default one)*/ BBReq.numIGs() - 1,
                               /*Baseline=*/size_t{0},
                               /*Uniformity=*/0.0, Alignment);

  assert(PlainGroupsSizes.size() == BBReq.numIGs() - 1);
  planning::BasicBlockRequest NewBBReq(BBReq.getMBB());

  auto AddPlainGroup =
      [&](size_t GroupSize, auto &Filter) {
        auto GenPolicy =
            planning::createGenPolicy(ProgCtx, Cfg.DefFlowConfig, Filter);
        if (DefaultGroupLimit.isNumLimit()) {
          auto Lim = planning::RequestLimit::NumInstrs{GroupSize};
          NewBBReq.add(planning::InstructionGroupRequest(std::move(Lim),
                                                         std::move(GenPolicy)));
          return;
        }
        auto Lim = planning::RequestLimit::Size{GroupSize};
        NewBBReq.add(planning::InstructionGroupRequest(std::move(Lim),
                                                       std::move(GenPolicy)));
      };

  // Add N plain groups interleaving with non-plain groups
  auto PlainGroupSizeIt = PlainGroupsSizes.begin();
  // We need delete one default group because now we are dividing it into small
  // parts and inserting them between other groups.
  auto GroupsWithoutDefault = drop_end(BBReq);
  for (auto &&Group : GroupsWithoutDefault) {
    auto Filter = Group.getOpcodeFilter();
    auto NumIGs = Group.numIGs();
    NewBBReq.add(std::move(Group));

    assert((NumIGs != 0) || *PlainGroupSizeIt == 0);
    for (auto Idx = 0u; Idx < NumIGs; ++Idx) {
      assert(PlainGroupSizeIt != PlainGroupsSizes.end());
      auto PlainGroupSize = *PlainGroupSizeIt;
      ++PlainGroupSizeIt;
      if (PlainGroupSize != 0 || NewBBReq.empty())
        AddPlainGroup(PlainGroupSize, Filter);
    }
  }
  assert(PlainGroupSizeIt == PlainGroupsSizes.end() &&
         "We iterate not over all groups!");

  // New limit must be identical to the old one, otherwise limit in
  // FunctionRequest will be incorrect
  assert(NewBBReq.limit() == BBReq.limit());
  BBReq = std::move(NewBBReq);
}

template <typename Predicate>
void FunctionRequestWrapper::initUnfilledBlocks(GeneratorContext *GenCtx,
                                                const FunctionGenerator *FG,
                                                const MachineFunction &MF,
                                                Predicate &&Pred) {
  auto MapRange = map_range(MF, [](auto &MBB) { return &MBB; });
  auto DropBlock = [&] {
    // Call without update because we drop this block
    call(&FunctionRequest::add, *MapRange.begin(),
         planning::BasicBlockRequest(*MapRange.begin()));
    MapRange = drop_begin(MapRange);
  };

  auto IsRegsInit = GenCtx->getConfig().PassCfg.RegistersConfig.InitializeRegs;
  if (IsRegsInit && FG->isEntryFunction(MF))
    DropBlock();
  assert(UnfilledBlocks.empty());
  copy_if(std::move(MapRange), std::back_inserter(UnfilledBlocks),
          std::forward<Predicate>(Pred));
}

void BlockGenPlanningImpl::processFunctionWithNumInstr(
    const MachineFunction &MF) {
  assert(GenCtx->getConfig().getGenerationMode() == GenerationMode::NumInstrs);

  auto LatchBlocks = collectLatchBlocks(*GenCtx, *MLI, MF, SimCtx);
  FunBlocks.initUnfilledBlocks(GenCtx, FG, MF, [&LatchBlocks](const auto *MBB) {
    return !LatchBlocks.count(MBB);
  });
  assert(!unfilledBlocks().empty() &&
         "At least one basic block that is not a latch block must exist");

  auto NumInstrTotal = FG->getRequestedInstrNum(MF);
  auto NumInstrsLeft = NumInstrTotal;
  assert(NumInstrTotal >= FG->getCFInstrNum(MF));
  NumInstrsLeft -= FG->getCFInstrNum(MF);

  FunBlocks.setAverageBlockLimit(NumInstrsLeft);
  NumInstrsLeft -= fillReqWithContextModeChanges(NumInstrsLeft);
  NumInstrsLeft -= fillReqWithBurstGroups(NumInstrsLeft, NumInstrTotal);

  if (GenCtx->getConfig().PassCfg.SMC.has_value() &&
      MF.getName() != SMCManagerT::SMCSrcFuncName &&
      MF.getName() != SMCManagerT::SMCTgtFuncName) {
    fillReqWithSMC(FG->getSMCCopyFuncDecl(), FG->getSMCTgtFunc());
  }

  fillReqWithPlainInsts<RequestLimit::NumInstrs>(NumInstrsLeft,
                                                 /* Alignment */ 1);

  for (auto *MBB : LatchBlocks)
    FunBlocks.callWithUpdate(&FunctionRequest::add, MBB,
                             planning::BasicBlockRequest(MBB));
}

void BlockGenPlanningImpl::processFunctionWithSize(const MachineFunction &MF) {
  assert(GenCtx->getConfig().getGenerationMode() == GenerationMode::Size);
  FunBlocks.initUnfilledBlocks(GenCtx, FG, MF, [](auto *MBB) { return true; });
  assert(!unfilledBlocks().empty() && "At least one basic block must exist");

  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  size_t MinInstrSize = getMinInstrSize(*MF.begin(), SnippyTgt);

  size_t SizeLeft = calculateMFSizeLimit(MF);
  if (SizeLeft % MinInstrSize != 0) {
    SizeLeft = llvm::alignDown(SizeLeft, MinInstrSize);
    warn(WarningName::IndivisibleSizeLimitSection,
         Twine("The given memory region size is not aligned to the minimum "
               "instruction size (") +
             Twine(MinInstrSize) + Twine(")"),
         "Rounding down to the nearest multiple");
  }

  assert(!SnippyTgt.needToGenerateModeSwitches(ProgCtx));
  assert(SizeLeft % MinInstrSize == 0);
  FunBlocks.setAverageBlockLimit(SizeLeft);
  fillReqWithPlainInsts<RequestLimit::Size>(SizeLeft, MinInstrSize);
}

static size_t calcFilledSize(FunctionRequestWrapper &FunBlocks,
                             ArrayRef<const MachineBasicBlock *> Blocks,
                             const SnippyTarget &SnpTgt, LLVMState &State) {
  size_t FilledSize = 0;
  for (auto *Block : Blocks) {
    FilledSize += State.getMBBSize(*Block);
    if (FunBlocks.call(&FunctionRequest::contains, Block)) {
      auto &Limit = FunBlocks.call(&FunctionRequest::get, Block).limit();
      assert(Limit.isSizeLimit());
      FilledSize += Limit.getLimit();
    }
  }
  return FilledSize;
}

static void setSizeForLoopBlock(FunctionRequestWrapper &FunBlocks,
                                const MachineBasicBlock &SelectedMBB,
                                ArrayRef<const MachineBasicBlock *> LoopBlocks,
                                NumericRange<ProgramCounterType> PCDist,
                                bool IsLatch, GeneratorContext &SGCtx) {
  assert(!FunBlocks.call(&FunctionRequest::contains, &SelectedMBB));
  auto &ProgCtx = SGCtx.getProgramContext();
  const auto &Cfg = SGCtx.getConfig();
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

  size_t FilledSize = calcFilledSize(FunBlocks, LoopBlocks, SnpTgt, State);
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
    const auto &Branches = SGCtx.getConfig().PassCfg.Branches;
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

  auto MinInstrSize = getMinInstrSize(SelectedMBB, SnpTgt);
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

  auto BlockSize = RandEngine::genInRangeInclusive(Min, Max);
  BlockSize = alignDown(BlockSize, MinInstrSize);
  auto Limit = planning::RequestLimit::Size{BlockSize};
  // InitialAmount allows to account for any already generated instructions
  auto InitialAmount =
      GenerationStatistics{NumOfPrimaryInstrs, /*GeneratedSize*/ MBBSize};
  auto GenPolicy = planning::createGenPolicy(ProgCtx, Cfg.DefFlowConfig);
  FunBlocks.call(
      &FunctionRequest::addToBlock<InstructionGroupRequest>, &SelectedMBB,
      planning::InstructionGroupRequest(std::move(Limit), std::move(GenPolicy),
                                        std::move(InitialAmount)));
}

void BlockGenPlanningImpl::fillReqForTopLoopBySize(const MachineLoop &ML) {
  assert(ML.isOutermost() && "Only top level loop expected");
  auto &ProgCtx = GenCtx->getProgramContext();
  if (!ML.getSubLoops().empty())
    fatal(ProgCtx.getLLVMState().getCtx(), "Block generation planning failed",
          "PC distance is now supported with max loop depth 1");

  auto PCDist = GenCtx->getConfig().PassCfg.Branches.getPCDistance();

  auto LoopBlocks = ML.getBlocks();
  for (auto *MBB : LoopBlocks)
    setSizeForLoopBlock(FunBlocks, *MBB, LoopBlocks, PCDist,
                        ML.isLoopLatch(MBB), *GenCtx);
}

void BlockGenPlanningImpl::processFunctionMixed(const MachineFunction &MF) {
  const auto &Cfg = GenCtx->getConfig();
  assert(Cfg.getGenerationMode() == GenerationMode::Mixed);

  // Process blocks out of loops
  FunBlocks.initUnfilledBlocks(GenCtx, FG, MF, [this](const auto *MBB) {
    return !MLI->getLoopFor(MBB);
  });
  unsigned SupposedNumInstr = 0;
  auto &ProgCtx = GenCtx->getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto MaxInstrSize = SnippyTgt.getMaxInstrSize();
  for (auto *ML : *MLI) {
    assert(ML);
    fillReqForTopLoopBySize(*ML);
    for (auto *MBB : ML->blocks()) {
      auto &Limit = FunBlocks.call(&FunctionRequest::get, MBB).limit();
      assert(Limit.isSizeLimit());
      auto BBSize = Limit.getLimit();
      SupposedNumInstr += llvm::alignTo(BBSize, MaxInstrSize) / MaxInstrSize;
    }
  }

  auto NumInstrTotal = FG->getRequestedInstrNum(MF);
  assert(NumInstrTotal >= FG->getCFInstrNum(MF));
  NumInstrTotal -= FG->getCFInstrNum(MF);
  // If number of instructions in size-requested blocks is already enough for
  // the whole function, skipping num instrs planning for other blocks
  if (NumInstrTotal <= SupposedNumInstr) {
    FunBlocks.fillEmptyBlocks();
    return;
  }
  auto NumInstrsLeft = NumInstrTotal - SupposedNumInstr;

  FunBlocks.setAverageBlockLimit(NumInstrsLeft);
  NumInstrsLeft -= fillReqWithContextModeChanges(NumInstrsLeft);
  NumInstrsLeft -= fillReqWithBurstGroups(NumInstrsLeft, NumInstrTotal);

  fillReqWithPlainInsts<RequestLimit::NumInstrs>(NumInstrsLeft,
                                                 /* Alignment */ 1);
}

static void checkGenModeCompatibility(GeneratorContext &GenCtx,
                                      const MachineLoopInfo &MLI,
                                      SimulatorContext &SimCtx) {
  auto GM = GenCtx.getConfig().getGenerationMode();
  if (GM == GenerationMode::NumInstrs)
    return;

  auto &ProgCtx = GenCtx.getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  if (SnippyTgt.needToGenerateModeSwitches(ProgCtx))
    snippy::fatal("Generation by size with rvv is not supported yet");

  bool LoopGenerated = !MLI.empty();
  bool TrackingEnabled = SimCtx.hasTrackingMode();
  if (LoopGenerated && TrackingEnabled)
    snippy::fatal(
        "Generation by size with loops in tracking mode is not supported");
}

void BlockGenPlanningImpl::processFunction(const MachineFunction &MF) {
  assert(GenCtx && MLI && FG);
  checkGenModeCompatibility(*GenCtx, *MLI, SimCtx);
  switch (GenCtx->getConfig().getGenerationMode()) {
  case GenerationMode::NumInstrs:
    return processFunctionWithNumInstr(MF);
  case GenerationMode::Size:
    return processFunctionWithSize(MF);
  case GenerationMode::Mixed:
    return processFunctionMixed(MF);
  }
  llvm_unreachable("unknown generation mode");
}

bool BlockGenPlanning::runOnMachineFunction(MachineFunction &MF) {
  auto *GenCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  auto *MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  auto *GenPlanWrapper = &getAnalysis<BlockGenPlanWrapper>();
  auto *FG = &getAnalysis<FunctionGenerator>();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();

  planning::FunctionRequest FunReq(MF, *GenCtx);
  BlockGenPlanningImpl Impl(GenCtx, MLI, FG, SimCtx, FunReq);
  Impl.processFunction(MF);
  GenPlanWrapper->setFunctionRequest(&MF, std::move(FunReq));

  return true;
}

} // namespace snippy
} // namespace llvm
