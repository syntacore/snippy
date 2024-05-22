//===-- GenerationUtils.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Generation.h"
#include "snippy/Generator/Backtrack.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Support/Options.h"

namespace llvm {
namespace snippy {
template <typename InstrItType> struct SelfcheckAnnotationInfo {
  InstrItType Inst{};
  Register DestReg;

  SelfcheckAnnotationInfo(InstrItType Inst, Register Reg)
      : Inst(Inst), DestReg(Reg) {}
};

template <typename InstrItType> struct SelfcheckFirstStoreInfo {
  InstrItType FirstStoreInstrPos;
  MemAddr Address;

  SelfcheckFirstStoreInfo(InstrItType InstrPos, MemAddr Address)
      : FirstStoreInstrPos(InstrPos), Address(Address) {}
};

template <typename InstrItType> struct SelfcheckIntermediateInfo {
  InstrItType NextInsertPos;
  SelfcheckFirstStoreInfo<InstrItType> FirstStoreInfo;
};
} // namespace snippy

template <typename InstrItType>
struct DenseMapInfo<snippy::SelfcheckAnnotationInfo<InstrItType>> {
  static inline snippy::SelfcheckAnnotationInfo<InstrItType> getEmptyKey() {
    return {InstrItType{}, ~0U};
  }
  static inline snippy::SelfcheckAnnotationInfo<InstrItType> getTombstoneKey() {
    return {InstrItType{}, ~0U - 1};
  }
  static unsigned
  getHashValue(const snippy::SelfcheckAnnotationInfo<InstrItType> &Val) {
    return Val.DestReg * 37U;
  }

  static bool isEqual(const snippy::SelfcheckAnnotationInfo<InstrItType> &LHS,
                      const snippy::SelfcheckAnnotationInfo<InstrItType> &RHS) {
    return LHS.DestReg == RHS.DestReg;
  }
};

namespace snippy {

#define DEBUG_TYPE "instruction generation"

extern cl::OptionCategory Options;

static snippy::opt<bool>
    GenerateInsertionPointHints("generate-insertion-point-hints",
                                cl::desc("Generate hint instructions in places "
                                         "where it's safe to mutate snippet"),
                                cl::Hidden, cl::init(false));
static snippy::opt<unsigned>
    BTThreshold("bt-threshold",
                cl::desc("the maximal number of back-tracking events"),
                cl::Hidden, cl::init(10000));

static snippy::opt<unsigned> SizeErrorThreshold(
    "size-error-threshold",
    cl::desc("the maximal number of attempts to generate code fitting size"),
    cl::Hidden, cl::init(5));

// FIXME: The only reason to change policy is if some instructions in the group
// require some initialization first. We switch to default policy that has these
// special instructions (overrides).
static void switchPolicyIfNeeded(planning::InstructionGroupRequest &IG,
                                 unsigned Opcode, MachineBasicBlock &MBB,
                                 GeneratorContext &GC) {
  auto &Tgt = GC.getLLVMState().getSnippyTarget();
  if (Tgt.needsGenerationPolicySwitch(Opcode))
    IG.changePolicy(
        planning::DefaultGenPolicy(GC, Tgt.getDefaultPolicyFilter(MBB, GC),
                                   Tgt.groupMustHavePrimaryInstr(MBB, GC),
                                   Tgt.getPolicyOverrides(MBB, GC)));
}

static void generateInsertionPointHint(
    planning::InstructionGenerationContext &InstrGenCtx) {
  auto &State = InstrGenCtx.GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  SnippyTgt.generateNop(InstrGenCtx.MBB, InstrGenCtx.Ins, State);
}

template <typename IteratorType>
size_t getCodeSize(IteratorType Begin, IteratorType End) {
  return std::accumulate(Begin, End, 0, [](size_t CurrentSize, const auto &MC) {
    size_t InstrSize = MC.getDesc().getSize();
    if (InstrSize == 0)
      errs() << "warning: Instruction has unknown size, "
                "the size calculation will be wrong.\n";
    return CurrentSize + InstrSize;
  });
}

template <typename IteratorType>
size_t countPrimaryInstructions(IteratorType Begin, IteratorType End) {
  return std::count_if(
      Begin, End, [](const auto &MI) { return !checkSupportMetadata(MI); });
}

enum class GenerationStatus {
  Ok,
  BacktrackingFailed,
  InterpretFailed,
  SizeFailed,
};

std::unique_ptr<Backtrack> createBacktrack(GeneratorContext &GC) {
  return std::make_unique<Backtrack>(GC.getOrCreateInterpreter());
}

bool preInterpretBacktracking(const MachineInstr &MI, GeneratorContext &GC) {
  auto BT = createBacktrack(GC);
  assert(BT);
  if (!BT->isInstrValid(MI, GC.getLLVMState().getSnippyTarget()))
    return false;
  // TODO: maybe there will be another one way for back-tracking?
  return true;
}

template <typename InstrIt>
GenerationStatus interpretInstrs(InstrIt Begin, InstrIt End,
                                 GeneratorContext &GC) {
  auto &I = GC.getOrCreateInterpreter();
  const auto &State = GC.getLLVMState();
  assert(GC.hasTrackingMode());

  for (auto &MI : iterator_range(Begin, End)) {
    if (GC.getGenSettings().TrackingConfig.BTMode &&
        !preInterpretBacktracking(MI, GC))
      return GenerationStatus::BacktrackingFailed;
    I.addInstr(MI, State);
    if (I.step() != ExecutionResult::Success)
      return GenerationStatus::InterpretFailed;
  }
  return GenerationStatus::Ok;
}

// TODO: We need other ways of this function implemetation
template <typename InstrIt>
std::vector<InstrIt> collectSelfcheckCandidates(
    InstrIt Begin, InstrIt End, GeneratorContext &GC,
    AsOneGenerator<bool, true, false> &SelfcheckPeriodTracker) {
  const auto &ST = GC.getLLVMState().getSnippyTarget();
  std::vector<InstrIt> FilteredCandidates;
  std::copy_if(Begin, End, std::back_inserter(FilteredCandidates),
               [](const auto &MI) { return !checkSupportMetadata(MI); });
  std::vector<InstrIt> Candidates;

  for (auto &PrimInstrIt : FilteredCandidates) {
    if (!SelfcheckPeriodTracker())
      continue;
    if (!ST.isSelfcheckAllowed(PrimInstrIt->getOpcode())) {
      errs() << "Selfcheck is not supported for this instruction\n";
      PrimInstrIt->print(errs());
      errs() << "NOTE: for RVV instructions you need to use "
                "--enable-selfcheck-rvv option\n";
      continue;
    }
    Candidates.push_back(PrimInstrIt);
  }
  return Candidates;
}

template <typename InstrIt>
void reportGeneratorRollback(InstrIt ItBegin, InstrIt ItEnd) {
  LLVM_DEBUG(
      dbgs() << "Generated instructions: \n"; for (auto CurInstr = ItBegin;
                                                   CurInstr != ItEnd;
                                                   ++CurInstr) {
        CurInstr->print(dbgs());
        dbgs().flush();
      } dbgs() << "are not valid. Regeneration...\n";);
}

template <typename InstrIt>
void storeRefValue(MachineBasicBlock &MBB, InstrIt InsertPos, MemAddr Addr,
                   APInt Val, RegPoolWrapper &RP, GeneratorContext &GC) {
  GC.getLLVMState().getSnippyTarget().storeValueToAddr(MBB, InsertPos, Addr,
                                                       Val, RP, GC);
}

void selfcheckOverflowGuard(const SectionDesc &SelfcheckSection,
                            unsigned long long SCAddress) {
  if (SCAddress >= SelfcheckSection.VMA + SelfcheckSection.Size)
    report_fatal_error("Selfcheck section overflow. Try to provide "
                       "\"selfcheck\" section description in layout file",
                       false);
}
// Returns iterator to the first inserted instruction. So [returned It;
// InsertPos) contains inserted instructions.
// Register pool is taken by value as we'll do some local reservations on top
// of the existing register pool. All changes made must be canceled at the
// function exit.
template <typename InstrIt>
SelfcheckIntermediateInfo<InstrIt> storeRefAndActualValueForSelfcheck(
    InstrIt InsertPos, Register DestReg, RegPoolWrapper RP,
    planning::InstructionGenerationContext &InstrGenCtx) {
  auto &GC = InstrGenCtx.GC;
  auto &MBB = InstrGenCtx.MBB;
  assert(InstrGenCtx.SelfCheck);
  auto &SCAddress = InstrGenCtx.SelfCheck->CurrentAddress;
  const auto &ST = GC.getLLVMState().getSnippyTarget();
  auto &I = GC.getOrCreateInterpreter();
  auto RegValue = I.readReg(DestReg);

  bool IsBegin = MBB.begin() == InsertPos;
  auto FirstInserted = InsertPos;
  if (!IsBegin)
    FirstInserted = std::prev(InsertPos);

  ST.storeRegToAddr(MBB, InsertPos, SCAddress, DestReg, RP, GC,
                    /* store the whole register */ 0);
  SelfcheckFirstStoreInfo<InstrIt> FirstStoreInfo{std::prev(InsertPos),
                                                  SCAddress};
  SCAddress += GC.getSCStride();
  auto &SelfcheckSection = GC.getSelfcheckSection();
  selfcheckOverflowGuard(SelfcheckSection, SCAddress);
  storeRefValue(MBB, InsertPos, SCAddress, RegValue, RP, GC);
  SCAddress += GC.getSCStride();
  selfcheckOverflowGuard(SelfcheckSection, SCAddress);


  if (IsBegin)
    return {MBB.begin(), FirstStoreInfo};
  return {std::next(FirstInserted), FirstStoreInfo};
}

struct GenerationResult {
  GenerationStatus Status;
  GenerationStatistics Stats;
};

template <typename InstrIt>
GenerationResult handleGeneratedInstructions(
    InstrIt ItBegin, planning::InstructionGenerationContext &InstrGenCtx,
    const planning::InstructionGroupRequest &IG,
    const GenerationStatistics &CurrentInstructionGroupStats) {
  auto &MBB = InstrGenCtx.MBB;
  auto &GC = InstrGenCtx.GC;
  auto ItEnd = InstrGenCtx.Ins;
  auto GeneratedCodeSize = getCodeSize(ItBegin, ItEnd);
  auto PrimaryInstrCount = countPrimaryInstructions(ItBegin, ItEnd);
  auto ReportGenerationResult =
      [ItBegin, ItEnd, &GeneratedCodeSize, PrimaryInstrCount,
       &MBB](GenerationStatus Code) -> GenerationResult {
    if (Code == GenerationStatus::Ok)
      return {/* Status */ Code, {PrimaryInstrCount, GeneratedCodeSize}};
    if (Code == GenerationStatus::InterpretFailed)
      return {/* Status */ Code, {0, GeneratedCodeSize}};

    LLVM_DEBUG(reportGeneratorRollback(ItBegin, ItEnd));
    MBB.erase(ItBegin, ItEnd);
    return {/* Status */ Code, /* Stats */ {0, 0}};
  };

  // Check size requirements before any backtrack execution
  if (sizeLimitIsReached(IG.limit(), CurrentInstructionGroupStats,
                         GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  if (!GC.hasTrackingMode())
    return ReportGenerationResult(GenerationStatus::Ok);

  auto InterpretInstrsResult = interpretInstrs(ItBegin, ItEnd, GC);
  if (InterpretInstrsResult != GenerationStatus::Ok)
    return ReportGenerationResult(InterpretInstrsResult);

  const auto &GenSettings = GC.getGenSettings();
  if (!GenSettings.TrackingConfig.SelfCheckPeriod)
    return ReportGenerationResult(GenerationStatus::Ok);

  if (PrimaryInstrCount == 0) {
    LLVM_DEBUG(
        dbgs() << "Ignoring selfcheck for the supportive instruction\n";);
    return ReportGenerationResult(GenerationStatus::Ok);
  }

  assert(PrimaryInstrCount >= 1);

  assert(InstrGenCtx.SelfCheck);
  // Collect instructions that can and should be selfchecked.
  auto SelfcheckCandidates = collectSelfcheckCandidates(
      ItBegin, ItEnd, GC, InstrGenCtx.SelfCheck->PeriodTracker);

  // Collect def registers from the candidates.
  // Use SetVector as we want to preserve order of insertion.
  SetVector<SelfcheckAnnotationInfo<InstrIt>> Defs;
  const auto &ST = GC.getLLVMState().getSnippyTarget();
  // Reverse traversal as we need to sort definitions by their last use.
  for (auto Inst : reverse(SelfcheckCandidates)) {
    // TODO: Add self-check for instructions without definitions
    if (Inst->getDesc().getNumDefs() == 0)
      continue;
    auto Regs = ST.getRegsForSelfcheck(*Inst, MBB, GC);
    for (auto &Reg : Regs)
      Defs.insert({Inst, Reg});
  }

  // Insert self-check instructions after current batch of instructions. This
  // allows the result of the primary instruction to be first postprocessed,
  // and then stored.
  // For example, this is useful in cases when it is known that the result of
  // the primary instructions is an undefined value which mustn't be committed
  // to memory.
  // We'll insert selfcheck for registers in reverse order starting from the
  // last recently used register. Each insertion will happen right before the
  // previous one. This simplifies registers reservation as we don't have (and
  // shouldn't) 'unreserve' mechanism for a given register. Small example:
  //   Definitions to be selfchecked:
  //     r1 = ...
  //     r2 = ...
  //     r3 = ...
  //     some possible postprocessing
  //   After selfcheck insertion:
  //     r1 = ...
  //     r2 = ...
  //     r3 = ...
  //     some possible postprocessing
  //     r1 selfcheck  <---- inserted third, support instructions cannot use r2
  //                         and r3.
  //     r2 selfcheck  <---- inserted second, support instructions cannot use
  //                         r3, but can use r1.
  //     r3 selfcheck  <---- inserted first, support instruction can use r1, r2.
  auto InsPoint = ItEnd;
  auto RP = GC.getRegisterPool();
  std::vector<SelfcheckFirstStoreInfo<InstrIt>> StoresInfo;
  for (const auto &Def : Defs) {
    RP.addReserved(Def.DestReg);
    auto SelfcheckInterInfo = storeRefAndActualValueForSelfcheck(
        InsPoint, Def.DestReg, RP.push(), InstrGenCtx);
    InsPoint = SelfcheckInterInfo.NextInsertPos;
    // Collect information about first generated selfcehck store for a primary
    // instr
    StoresInfo.push_back(SelfcheckInterInfo.FirstStoreInfo);
  }

  assert(Defs.size() == StoresInfo.size());
  // Add collected information about generated selfcheck stores for selfcheck
  // annotation
  for (const auto &[Def, StoreInfo] : zip(Defs, StoresInfo))
    GC.addToSelfcheckMap(
        StoreInfo.Address,
        std::distance(Def.Inst, std::next(StoreInfo.FirstStoreInstrPos)));
  // Execute self-check instructions
  auto &I = GC.getOrCreateInterpreter();
  if (!I.executeChainOfInstrs(GC.getLLVMState(), InsPoint, ItEnd))
    I.reportSimulationFatalError(
        "Failed to execute chain of instructions in tracking mode");

  // Check size requirements after selfcheck addition.
  GeneratedCodeSize = getCodeSize(ItBegin, ItEnd);
  if (sizeLimitIsReached(IG.limit(), CurrentInstructionGroupStats,
                         GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  return ReportGenerationResult(GenerationStatus::Ok);
}

template <typename InstrIt>
MachineBasicBlock::iterator processGeneratedInstructions(
    InstrIt ItBegin, planning::InstructionGenerationContext &InstrGenCtx,
    const planning::InstructionGroupRequest &IG,
    GenerationStatistics &CurrentStats) {
  auto &MBB = InstrGenCtx.MBB;
  auto ItEnd = MBB.getFirstTerminator();
  ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
  auto IntRes =
      handleGeneratedInstructions(ItBegin, InstrGenCtx, IG, CurrentStats);
  processGenerationResult(IG, InstrGenCtx, IntRes, CurrentStats);
  CurrentStats.merge(IntRes.Stats);
  return std::prev(ItEnd);
}

static MachineOperand createRegAsOperand(Register Reg, unsigned Flags,
                                         unsigned SubReg = 0) {
  assert((Flags & 0x1) == 0 && "0x1 is not a RegState flag");
  return MachineOperand::CreateReg(
      Reg, Flags & RegState::Define, Flags & RegState::Implicit,
      Flags & RegState::Kill, Flags & RegState::Dead, Flags & RegState::Undef,
      Flags & RegState::EarlyClobber, SubReg, Flags & RegState::Debug,
      Flags & RegState::InternalRead, Flags & RegState::Renamable);
}

std::optional<MachineOperand> pregenerateOneOperand(
    const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP, const MCOperandInfo &MCOpInfo,
    const planning::PreselectedOpInfo &Preselected, unsigned OpIndex,
    ArrayRef<MachineOperand> PregeneratedOperands, GeneratorContext &GC) {
  auto OpType = MCOpInfo.OperandType;

  const auto &State = GC.getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RegGen = GC.getRegGen();
  auto Operand = InstrDesc.operands()[OpIndex];
  auto OperandRegClassID = Operand.RegClass;
  if (OpType == MCOI::OperandType::OPERAND_REGISTER) {
    assert(MCOpInfo.RegClass != -1);
    bool IsDst = Preselected.getFlags() & RegState::Define;
    Register Reg;
    if (Preselected.isTiedTo())
      Reg = PregeneratedOperands[Preselected.getTiedTo()].getReg();
    else if (Preselected.isReg())
      Reg = Preselected.getReg();
    else {
      auto RegClass = SnippyTgt.getRegClass(
          GC, OperandRegClassID, OpIndex, InstrDesc.getOpcode(), MBB, RegInfo);
      auto Exclude =
          SnippyTgt.excludeRegsForOperand(RegClass, GC, InstrDesc, OpIndex);
      auto Include = SnippyTgt.includeRegs(RegClass);
      AccessMaskBit Mask = IsDst ? AccessMaskBit::W : AccessMaskBit::R;

      auto CustomMask =
          SnippyTgt.getCustomAccessMaskForOperand(GC, InstrDesc, OpIndex);
      // TODO: add checks that target-specific and generic restrictions are
      // not conflicting.
      if (CustomMask != AccessMaskBit::None)
        Mask = CustomMask;
      auto RegOpt = RegGen.generate(RegClass, OperandRegClassID, RegInfo, RP,
                                    MBB, SnippyTgt, Exclude, Include, Mask);
      if (!RegOpt)
        return std::nullopt;
      Reg = RegOpt.value();
    }
    SnippyTgt.reserveRegsIfNeeded(InstrDesc.getOpcode(),
                                  /* isDst */ IsDst,
                                  /* isMem */ false, Reg, RP, GC, MBB);
    return createRegAsOperand(Reg, Preselected.getFlags());
  }
  if (OpType == MCOI::OperandType::OPERAND_MEMORY) {
    // FIXME: we expect memory operands to be registers
    assert(MCOpInfo.RegClass != -1);
    Register Reg;
    if (Preselected.isTiedTo())
      Reg = PregeneratedOperands[Preselected.getTiedTo()].getReg();
    else if (Preselected.isReg())
      Reg = Preselected.getReg();
    else {
      auto RegClass = RegInfo.getRegClass(MCOpInfo.RegClass);
      auto Exclude =
          SnippyTgt.excludeFromMemRegsForOpcode(InstrDesc.getOpcode());
      auto RegOpt = RegGen.generate(RegClass, OperandRegClassID, RegInfo, RP,
                                    MBB, SnippyTgt, Exclude);
      if (!RegOpt)
        return std::nullopt;
      Reg = RegOpt.value();
    }
    // FIXME: RW mask is too restrictive for the majority of instructions.
    SnippyTgt.reserveRegsIfNeeded(InstrDesc.getOpcode(),
                                  /* isDst */ Preselected.getFlags() &
                                      RegState::Define,
                                  /* isMem */ true, Reg, RP, GC, MBB);
    return createRegAsOperand(Reg, Preselected.getFlags());
  }

  if (OpType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
    assert(Preselected.isUnset() || Preselected.isImm());
    StridedImmediate StridedImm;
    if (Preselected.isImm())
      StridedImm = Preselected.getImm();
    if (StridedImm.isInitialized() &&
        StridedImm.getMin() == StridedImm.getMax())
      return MachineOperand::CreateImm(StridedImm.getMax());
    return SnippyTgt.generateTargetOperand(GC, InstrDesc.getOpcode(), OpType,
                                           StridedImm);
  }
  llvm_unreachable("this operand type unsupported");
}
std::optional<SmallVector<MachineOperand, 8>> tryToPregenerateOperands(
    const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP,
    const std::vector<planning::PreselectedOpInfo> &Preselected,
    GeneratorContext &GC) {
  SmallVector<MachineOperand, 8> PregeneratedOperands;
  assert(InstrDesc.getNumOperands() == Preselected.size());
  iota_range<unsigned long> PreIota(0, Preselected.size(),
                                    /* Inclusive */ false);
  for (const auto &[MCOpInfo, PreselOpInfo, Index] :
       zip(InstrDesc.operands(), Preselected, PreIota)) {
    auto OpOpt =
        pregenerateOneOperand(MBB, InstrDesc, RP, MCOpInfo, PreselOpInfo, Index,
                              PregeneratedOperands, GC);
    if (!OpOpt)
      return std::nullopt;
    PregeneratedOperands.push_back(OpOpt.value());
  }
  return PregeneratedOperands;
}

SmallVector<MachineOperand, 8>
pregenerateOperands(const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
                    RegPoolWrapper &RP,
                    const std::vector<planning::PreselectedOpInfo> &Preselected,
                    GeneratorContext &GC) {
  auto &RegGenerator = GC.getRegGen();
  auto &State = GC.getLLVMState();
  auto &InstrInfo = State.getInstrInfo();

  auto IterNum = 0u;
  constexpr auto FailsMaxNum = 10000u;

  while (IterNum < FailsMaxNum) {
    RP.reset();
    RegGenerator.setRegContextForPlugin();
    auto PregeneratedOperandsOpt =
        tryToPregenerateOperands(MBB, InstrDesc, RP, Preselected, GC);
    IterNum++;

    if (PregeneratedOperandsOpt)
      return std::move(PregeneratedOperandsOpt.value());
  }

  report_fatal_error("Limit on failed generation attempts exceeded "
                     "on instruction " +
                         InstrInfo.getName(InstrDesc.getOpcode()),
                     false);
}

static auto stringifyRequestStatus(GenerationStatus Status) {
  switch (Status) {
  case GenerationStatus::Ok:
    return "ok";
  case GenerationStatus::BacktrackingFailed:
    return "backtrack_failure";
  case GenerationStatus::InterpretFailed:
    return "interpret_failure";
  case GenerationStatus::SizeFailed:
    return "size_failure";
  }
  llvm_unreachable("unkown GenerationStatus");
}

static void printInterpretResult(raw_ostream &OS, const Twine &Prefix,
                                 const GenerationResult &TheResult) {
  OS << Prefix << "InterpretResult={ ";
  TheResult.Stats.print(OS);
  OS << ", RequestStatus: " << stringifyRequestStatus(TheResult.Status) << " }";
}

static GenerationResult generateNopsToSizeLimit(
    const planning::InstructionGroupRequest &InstructionGroupRequest,
    planning::InstructionGenerationContext &InstrGenCtx,
    GenerationStatistics &CurrentInstructionGroupStats) {
  assert(InstructionGroupRequest.limit().isSizeLimit());
  auto &GC = InstrGenCtx.GC;
  auto &MBB = InstrGenCtx.MBB;
  auto &State = GC.getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto ItEnd = MBB.getFirstTerminator();
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  while (!InstructionGroupRequest.limit().isReached(
      CurrentInstructionGroupStats)) {
    auto *MI = SnpTgt.generateNop(MBB, ItBegin, State);
    assert(MI && "Nop generation failed");
    CurrentInstructionGroupStats.merge(
        GenerationStatistics(0, MI->getDesc().getSize()));
  }
  ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
  return handleGeneratedInstructions(ItBegin, InstrGenCtx,
                                     InstructionGroupRequest,
                                     CurrentInstructionGroupStats);
}

bool sizeLimitIsReached(const planning::RequestLimit &Lim,
                        const GenerationStatistics &CommitedStats,
                        size_t NewGeneratedCodeSize) {
  if (!Lim.isSizeLimit())
    return false;
  return NewGeneratedCodeSize > Lim.getSizeLeft(CommitedStats);
}

AddressGenInfo chooseAddrGenInfoForInstrCallback(
    LLVMContext &Ctx,
    std::optional<GeneratorContext::LoopGenerationInfo> CurLoopGenInfo,
    size_t AccessSize, size_t Alignment, const MemoryAccess &MemoryScheme) {
  (void)CurLoopGenInfo; // for future extensibility
  // AddressGenInfo for one element access.
  return AddressGenInfo::singleAccess(AccessSize, Alignment, false /* Burst */);
}

std::pair<AddressInfo, AddressGenInfo>
chooseAddrInfoForInstr(MachineInstr &MI, GeneratorContext &SGCtx,
                       const MachineLoop *ML) {
  auto &State = SGCtx.getLLVMState();
  auto &MS = SGCtx.getMemoryScheme();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto Opcode = MI.getDesc().getOpcode();

  auto [AccessSize, Alignment] =
      SnippyTgt.getAccessSizeAndAlignment(Opcode, SGCtx, *MI.getParent());

  auto CurLoopGenInfo =
      ML ? SGCtx.getLoopsGenerationInfoForMBB(ML->getHeader()) : std::nullopt;

  // Depending on which memory scheme is chosen we either try to generate a
  // strided access if the scheme has [ind-var] attribute or fallback to
  // regular single address access
  auto ChooseAddrGenInfo =
      [&Ctx = State.getCtx(), CurLoopGenInfo, AccessSize = AccessSize,
       Alignment =
           Alignment](const MemoryAccess &MemoryScheme) -> AddressGenInfo {
    return chooseAddrGenInfoForInstrCallback(Ctx, CurLoopGenInfo, AccessSize,
                                             Alignment, MemoryScheme);
  };


  auto Result = MS.randomAddressForInstructions(AccessSize, Alignment,
                                                ChooseAddrGenInfo);
  const auto &AddrInfo = std::get<AddressInfo>(Result);

  assert(AddrInfo.MaxOffset >= 0);
  assert(AddrInfo.MinOffset <= 0);

  return Result;
}

SmallVector<unsigned, 4> pickRecentDefs(MachineInstr &MI,
                                        ArrayRef<unsigned> Regs,
                                        unsigned RegCountLimit,
                                        unsigned InstrLimit) {
  SmallVector<unsigned, 4> Picked;
  MachineInstr *CurMI = MI.getPrevNode();
  DenseSet<unsigned> PickedSet;
  for (auto i = 0u; i < InstrLimit && CurMI && Picked.size() < RegCountLimit;
       ++i) {
    for (auto &Op : CurMI->defs()) {
      auto OpReg = Op.getReg();
      if (PickedSet.count(OpReg))
        continue;
      if (std::none_of(Regs.begin(), Regs.end(),
                       [OpReg](auto &Reg) { return Reg == OpReg; }))
        continue;
      PickedSet.insert(OpReg);
      Picked.emplace_back(OpReg);
    }
    CurMI = CurMI->getPrevNode();
  }

  return Picked;
}

unsigned chooseAddressRegister(MachineInstr &MI, const AddressPart &AP,
                               RegPoolWrapper &RP, GeneratorContext &GC) {
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  if (AP.RegClass == nullptr)
    // It is not allowed to pick another register if RegClass is null.
    return AP.FixedReg;

  auto AllRegisters =
      RP.getAllAvailableRegisters(*AP.RegClass, *MI.getParent());
  auto Exclude = SnippyTgt.excludeFromMemRegsForOpcode(MI.getOpcode());

  erase_if(AllRegisters,
           [&](Register Reg) { return is_contained(Exclude, Reg); });

  if (AllRegisters.empty())
    // Sometimes all registers may happen to be reserved (Even AP.FixedReg).
    // In that case register is left as is.
    return AP.FixedReg;

  auto PickedRegisters =
      pickRecentDefs(MI, AllRegisters, 4 /* Register pick limit */,
                     20 /* Instruction search depth limit */);
  if (PickedRegisters.empty())
    // No recent defs of available registers found.
    return AP.FixedReg;

  // Find Register among PickedRegisters with minimal value transform
  // cost.
  std::pair<unsigned, int> MinimalMatCost = {0, -1};

  auto &I = GC.getOrCreateInterpreter();
  auto AddrValue = AP.Value;

  MinimalMatCost = std::accumulate(
      PickedRegisters.begin(), PickedRegisters.end(), MinimalMatCost,
      [&I, &SnippyTgt, &AddrValue, &GC](auto CurCost, auto &&Reg) {
        auto NewCost = CurCost;
        auto MatCost = SnippyTgt.getTransformSequenceLength(I.readReg(Reg),
                                                            AddrValue, Reg, GC);
        if (NewCost.second == -1 ||
            static_cast<unsigned>(NewCost.second) > MatCost) {
          NewCost.first = Reg;
          NewCost.second = MatCost;
        }

        return NewCost;
      });

  // Update corresponding MI operands with picked register.
  auto Reg = MinimalMatCost.first;
  for (auto &OpIdx : AP.operands) {
    auto &Op = MI.getOperand(OpIdx);
    assert(Op.isReg() && "Operand is not register");
    Op.setReg(Reg);
  }

  return Reg;
}
void postprocessMemoryOperands(MachineInstr &MI, RegPoolWrapper &RP,
                               GeneratorContext &GC) {
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto Opcode = MI.getDesc().getOpcode();
  auto NumAddrsToGen = SnippyTgt.countAddrsToGenerate(Opcode);
  auto *MLI = GC.getMachineLoopInfo();
  auto *MBB = MI.getParent();
  auto *ML = MLI ? MLI->getLoopFor(MBB) : nullptr;

  for (size_t i = 0; i < NumAddrsToGen; ++i) {
    auto [AddrInfo, AddrGenInfo] = chooseAddrInfoForInstr(MI, GC, ML);
    bool GenerateStridedLoad = !AddrGenInfo.isSingleElement();
    auto AccessSize = AddrInfo.AccessSize;

    if (GenerateStridedLoad && GC.hasTrackingMode())
      snippy::fatal(State.getCtx(), "Incompatible features",
                    "Can't use generate strided accesses in loops with "
                    "tracking mode enabled");

    auto &&[RegToValue, ChosenAddresses] =
        SnippyTgt.breakDownAddr(AddrInfo, MI, i, GC);

    for (unsigned j = 0; j < AddrGenInfo.NumElements; ++j) {
      addMemAccessToDump(ChosenAddresses, GC, AccessSize);

      for_each(ChosenAddresses,
               [Stride = AddrInfo.MinStride](auto &Val) { Val += Stride; });
    }

    auto InsPoint = MI.getIterator();
    for (auto &AP : RegToValue) {
      MCRegister Reg = AP.FixedReg;
      if (GC.hasTrackingMode() &&
          GC.getGenSettings().TrackingConfig.AddressVH) {
        auto &I = GC.getOrCreateInterpreter();
        Reg = chooseAddressRegister(MI, AP, RP, GC);
        SnippyTgt.transformValueInReg(*MI.getParent(), InsPoint, I.readReg(Reg),
                                      AP.Value, Reg, RP, GC);
      } else {
        SnippyTgt.writeValueToReg(*MI.getParent(), InsPoint, AP.Value, Reg, RP,
                                  GC);
      }
      // Address registers must not be overwritten during other memory operands
      // preparation.
      RP.addReserved(Reg, AccessMaskBit::W);
    }
  }
}

void generateCall(unsigned OpCode,
                  planning::InstructionGenerationContext &InstrGenCtx) {
  auto &GC = InstrGenCtx.GC;
  auto &MBB = InstrGenCtx.MBB;
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &CGS = GC.getCallGraphState();
  auto *Node = CGS.getNode(&(MBB.getParent()->getFunction()));
  auto CalleeCount = Node->callees().size();
  if (!CalleeCount)
    return;
  auto CalleeIdx = RandEngine::genInRange(CalleeCount);
  auto *CalleeNode = std::next(Node->callees().begin(), CalleeIdx)->Dest;
  auto FunctionIdx = RandEngine::genInRange(CalleeNode->functions().size());

  auto &CallTarget = *(CalleeNode->functions()[FunctionIdx]);

  SnippyTgt.generateCall(MBB, InstrGenCtx.Ins, CallTarget, GC,
                         /* AsSupport */ false, OpCode);
  Node->markAsCommitted(CalleeNode);
}

MachineInstr *
randomInstruction(const MCInstrDesc &InstrDesc,
                  std::vector<planning::PreselectedOpInfo> Preselected,
                  planning::InstructionGenerationContext &InstrGenCtx) {
  auto &MBB = InstrGenCtx.MBB;
  auto &GC = InstrGenCtx.GC;
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto MIB = BuildMI(MBB, InstrGenCtx.Ins, MIMetadata(), InstrDesc);

  // If any information about operands is provided from the caller, we won't do
  // any postprocessing. In this case the caller should be responsible for any
  // additional processing of the instruction.
  bool DoPostprocess = Preselected.empty();

  auto NumDefs = InstrDesc.getNumDefs();
  auto TotalNum = InstrDesc.getNumOperands();

  if (Preselected.empty())
    Preselected.resize(TotalNum);
  assert(Preselected.size() == TotalNum);
  for (auto &&[OpIdx, OpInfo] : enumerate(Preselected)) {
    OpInfo.setFlags(OpInfo.getFlags() |
                    (OpIdx < NumDefs ? RegState::Define : 0));
    auto TiedTo = InstrDesc.getOperandConstraint(OpIdx, MCOI::TIED_TO);
    if (TiedTo >= 0)
      OpInfo.setTiedTo(TiedTo);
  }

  auto RP = GC.getRegisterPool();
  auto PregeneratedOperands =
      pregenerateOperands(MBB, InstrDesc, RP, Preselected, GC);
  for (auto &Op : PregeneratedOperands)
    MIB.add(Op);

  if (DoPostprocess)
    postprocessMemoryOperands(*MIB, RP, GC);
  // FIXME:
  // We have a lot of problems with rollback and configurations
  // After this, we can have additional instruction after main one!
  auto PostProcessInstPos = std::next(MIB->getIterator());
  assert(PostProcessInstPos == InstrGenCtx.Ins);
  SnippyTgt.instructionPostProcess(*MIB, GC, PostProcessInstPos);
  return MIB;
}

void generateInstruction(const MCInstrDesc &InstrDesc,
                         planning::InstructionGenerationContext &InstrGenCtx,
                         std::vector<planning::PreselectedOpInfo> Preselected) {
  auto &State = InstrGenCtx.GC.getLLVMState();
  auto Opc = InstrDesc.getOpcode();
  const auto &SnippyTgt = State.getSnippyTarget();

  assert(!InstrDesc.isBranch() &&
         "All branch instructions expected to be generated separately");

  if (SnippyTgt.requiresCustomGeneration(InstrDesc)) {
    SnippyTgt.generateCustomInst(InstrDesc, InstrGenCtx);
    return;
  }

  if (SnippyTgt.isCall(Opc))
    generateCall(Opc, InstrGenCtx);
  else
    randomInstruction(InstrDesc, std::move(Preselected), InstrGenCtx);
}

MachineBasicBlock *findNextBlockOnModel(MachineBasicBlock &MBB,
                                        GeneratorContext &GC) {
  const auto &SnippyTgt = GC.getLLVMState().getSnippyTarget();
  auto &I = GC.getOrCreateInterpreter();
  auto PC = I.getPC();
  for (auto &Branch : MBB.terminators()) {
    assert(Branch.isBranch());
    if (Branch.isUnconditionalBranch())
      return SnippyTgt.getBranchDestination(Branch);

    assert(!Branch.isIndirectBranch() &&
           "Indirect branches are not supported for execution on the model "
           "during code generation.");
    assert(Branch.isConditionalBranch());
    I.addInstr(Branch, GC.getLLVMState());

    if (I.step() != ExecutionResult::Success)
      I.reportSimulationFatalError(
          "Fail to execute generated instruction in tracking mode");

    if (I.getPC() == PC)
      // pc + 0. Conditional branch is taken (Asm printer replaces BB's label
      // with zero because the actual offset is still unknown).
      return SnippyTgt.getBranchDestination(Branch);
  }

  return MBB.getNextNode();
}

MachineBasicBlock *
findNextBlock(MachineBasicBlock *MBB,
              const std::set<MachineBasicBlock *, MIRComp> &NotVisited,
              const MachineLoop *ML, GeneratorContext &GC) {
  if (GC.hasTrackingMode() && MBB) {
    // In selfcheck/backtracking mode we go to exit block of the loop right
    // after the latch block.
    if (ML && ML->getLoopLatch() == MBB)
      return ML->getExitBlock();
    return findNextBlockOnModel(*MBB, GC);
  }
  // When we're not tracking execution on the model or worrying about BBs order,
  // we can pick up any BB that we haven't processed yet.
  if (NotVisited.empty())
    return nullptr;
  return *NotVisited.begin();
}

template <typename RegsSnapshotTy>
void writeRegsSnapshot(RegsSnapshotTy RegsSnapshot, MachineBasicBlock &MBB,
                       RegStorageType Storage, GeneratorContext &GC) {
  const auto &SnippyTgt = GC.getLLVMState().getSnippyTarget();
  auto RP = GC.getRegisterPool();
  for (auto &&[RegIdx, Value] : RegsSnapshot) {
    auto Reg = SnippyTgt.regIndexToMCReg(RegIdx, Storage, GC);
    // FIXME: we expect that writeValueToReg won't corrupt other registers of
    // the same class even if they were not reserved. Also we expect that
    // writing to FP/V register may use only non-reserved GPR registers.
    SnippyTgt.writeValueToReg(MBB, MBB.getFirstTerminator(),
                              toAPInt(Value, SnippyTgt.getRegBitWidth(Reg, GC)),
                              Reg, RP, GC);
  }
}

GenerationStatistics generateCompensationCode(MachineBasicBlock &MBB,
                                              GeneratorContext &GC) {
  assert(GC.hasTrackingMode());

  const auto &SnippyTgt = GC.getLLVMState().getSnippyTarget();

  auto &I = GC.getOrCreateInterpreter();
  auto MemSnapshot = I.getMemBeforeTransaction();
  auto FRegsSnapshot = I.getFRegsBeforeTransaction();
  auto VRegsSnapshot = I.getVRegsBeforeTransaction();
  // Restoring memory requires GPR registers to prepare address and value to
  // write, so we need know which registers were changed. We cannot do it in the
  // current opened transaction as we'll mess up exiting from the loop state.
  // So, open a new one.
  I.openTransaction();
  if (!MemSnapshot.empty()) {
    auto StackSec = GC.getStackSection();
    auto SelfcheckSec = GC.getSelfcheckSection();
    for (auto [Addr, Data] : MemSnapshot) {
      assert(StackSec.Size > 0 && "Stack section cannot have zero size");
      // Skip stack and selfcheck memory. None of them can be rolled back.
      if (std::clamp<size_t>(Addr, StackSec.VMA,
                             StackSec.VMA + StackSec.Size) == Addr ||
          std::clamp<size_t>(Addr, SelfcheckSec.VMA,
                             SelfcheckSec.VMA + SelfcheckSec.Size) == Addr)
        continue;
      auto RP = GC.getRegisterPool();
      SnippyTgt.storeValueToAddr(
          MBB, MBB.getFirstTerminator(), Addr,
          {sizeof(Data) * CHAR_BIT, static_cast<unsigned char>(Data)}, RP, GC);
    }
  }

  writeRegsSnapshot(FRegsSnapshot, MBB, RegStorageType::FReg, GC);
  writeRegsSnapshot(VRegsSnapshot, MBB, RegStorageType::VReg, GC);
  // Execute inserted instructions to get a change made by the opened
  // transaction.
  if (snippy::interpretInstrs(MBB.begin(), MBB.getFirstTerminator(), GC) !=
      GenerationStatus::Ok)
    report_fatal_error("Inserted compensation code is incorrect", false);

  // We have two active transactions for which compensation code for GPRs must
  // be inserted at this point. The most inner one is a transaction opened in
  // the latch block to restore the memory state. And the previous transaction
  // is the main transaction opened at the loop header.
  // For example let's say that we have a simple loop:
  //   Preheader
  //      | ______
  //      |/      \
  //      v       |
  //   Header     |
  //      |       |
  //      v     Latch
  //   Exiting    ^
  //      |\______/
  //      v
  //    Exit
  //
  // We opened the first transaction right before Header to keep the state
  // change to be able to insert compensation code in the latch block. Now let's
  // take a more detailed look at Latch. When we entered Latch we know which
  // registers and memory we need to roll-back. To roll-back memory, FP and V
  // registers we need additional X registers to form addresses, intermediate
  // values and so on. Let's say in the loop body we have only three
  // instructions:
  //   Header:
  //      addi x1, x0, 0x10
  //      addi x2, x0, 0x1
  //      sd x2, 0(x1)    <---- writes 0x1 at addr 0x10.
  //      beq ...
  // Then in Latch we need to restore memory at addr 0x10 with value that was
  // before the store. Our transactions mechanism allows tracking previous
  // values and for example it'll be 0x5. Then we should insert the following
  // code:
  //   Latch block:
  //      addi xA, x0, 0x10
  //      addi xB, x0, 0x5
  //      sd xB, 0(xA)
  // In this example xA and xB are randomly chosen registers. As you can see,
  // we've restored memory and now should restore registers: x1, x2 ... and xA,
  // xB. Though, xA and xB haven't been tracked in transactions as they were not
  // executed. Let's execute them than.
  //    Latch block:
  //      addi xA, x0, 0x10
  //      addi xB, x0, 0x5
  //      sd xB, 0(xA)
  //       <----------- interpretation of the three instructions above
  // Know we know the whole set of X registers to restore and can insert
  // compensation code:
  //    Latch block:
  //      addi xA, x0, 0x10
  //      addi xB, x0, 0x5
  //      sd xB, 0(xA)
  //       <----------- interpretation of the three instructions above
  //      addi xA, x0, some prev value
  //      addi xB, x0, some prev value
  //      addi x1, x0, some prev value
  //      addi x2, x0, some prev value
  // Latch block is ready and we can go to the exit block and commit the
  // transaction, right? Well, not exactly. The state must match the one at loop
  // exit, but interpretation in the latch block added xA and xB to the
  // transaction with wrong values (0x10 and ox5). The solution is to hide
  // interpretation in Latch to one more transaction that we can simply discard.
  // Then final logic in Latch is the following:
  //    Latch block:
  //       <----------- open transaction to allow interpreting instruction in
  //       Latch
  //      addi xA, x0, 0x10
  //      addi xB, x0, 0x5
  //      sd xB, 0(xA)
  //       <----------- interpretation of the three instructions above to
  //       collect X regs to restore
  //      addi xA, x0, some prev value
  //      addi xB, x0, some prev value
  //       <----------- x regs changed in the last transaction restored, discard
  //       the transaction
  //       <----------- drop the trasnaction (commit the empty one)
  //      addi x1, x0, some prev value
  //      addi x2, x0, some prev value
  //       <----------- x regs changed in the main transaction (opened right
  //       before Header) restored
  //
  // It may not be the most optimal solution, but generic enough and rather
  // simple. To minimize compensation code we can do simple analyzes to track
  // that we can re-use x1 and don't use xA at all. Also it's possible that
  // we'll restore some registers twice. All these mentioned might be the field
  // for futher improvements.
  auto XRegsCompCode = I.getXRegsBeforeTransaction();
  I.discardTransaction();
  I.commitTransaction();
  auto XRegsLoopBody = I.getXRegsBeforeTransaction();
  auto WholeXRegsSnapshot =
      concat<decltype(XRegsCompCode)::value_type>(XRegsCompCode, XRegsLoopBody);
  writeRegsSnapshot(WholeXRegsSnapshot, MBB, RegStorageType::XReg, GC);

  return GenerationStatistics(
      0, getCodeSize(MBB.begin(), MBB.getFirstTerminator()));
}

void finalizeFunction(MachineFunction &MF, planning::FunctionRequest &Request,
                      const GenerationStatistics &MFStats, GeneratorContext &GC,
                      SelfCheckInfo *SelfCheckInfo) {
  auto &State = GC.getLLVMState();
  auto &MBB = MF.back();

  auto LastInstr = GC.getLastInstr();
  bool NopLastInstr = LastInstr.empty();

  // Secondary functions always return.
  if (!GC.isRootFunction(MF)) {
    State.getSnippyTarget().generateReturn(MBB, State);
    return;
  }

  // Root functions are connected via tail calls.
  if (!GC.isExitFunction(MF)) {
    State.getSnippyTarget().generateTailCall(MBB, *GC.nextRootFunction(MF), GC);
    return;
  }

  auto *ExitSym =
      MF.getContext().getOrCreateSymbol(Linker::GetExitSymbolName());

  // User may ask for last instruction to be return.
  if (GC.useRetAsLastInstr()) {
    State.getSnippyTarget()
        .generateReturn(MBB, State)
        ->setPreInstrSymbol(MF, ExitSym);
    return;
  }

  // Or to generate nop
  if (NopLastInstr) {
    State.getSnippyTarget().generateNop(MBB, MBB.end(), State);
    MBB.back().setPostInstrSymbol(MF, ExitSym);
    return;
  }
  for (auto &&FinalReq : Request.getFinalGenReqs(MFStats)) {
    assert((!FinalReq.limit().isNumLimit() || FinalReq.limit().getLimit()) &&
           "FinalReq is empty!");
    auto RP = GC.getRegisterPool();
    GenerationStatistics Stats;
    planning::InstructionGenerationContext InstrGenCtx{
        MF.back(), MF.back().end(), &RP, GC, SelfCheckInfo};
    snippy::generate(FinalReq, Stats, InstrGenCtx);
  }

  // Mark last generated instruction as support one.
  setAsSupportInstr(*(--MBB.getFirstTerminator()), State.getCtx());
  MBB.back().setPreInstrSymbol(MF, ExitSym);
}

void processGenerationResult(
    const planning::InstructionGroupRequest &IG,
    planning::InstructionGenerationContext &InstrGenCtx,
    const GenerationResult &IntRes, GenerationStatistics &CurrentStats) {
  LLVM_DEBUG(printInterpretResult(dbgs(), "      ", IntRes); dbgs() << "\n");
  auto &GC = InstrGenCtx.GC;
  auto &BacktrackCount = InstrGenCtx.BacktrackCount;
  auto &SizeErrorCount = InstrGenCtx.SizeErrorCount;
  switch (IntRes.Status) {
  case GenerationStatus::InterpretFailed: {
    GC.getOrCreateInterpreter().reportSimulationFatalError(
        "Fail to execute generated instruction in tracking mode\n");
  }
  case GenerationStatus::BacktrackingFailed: {
    ++BacktrackCount;
    if (BacktrackCount < BTThreshold)
      return;
    report_fatal_error("Back-tracking events threshold is exceeded", false);
  }
  case GenerationStatus::SizeFailed: {
    ++SizeErrorCount;
    if (SizeErrorCount < SizeErrorThreshold)
      return;

    // We have size error too many times, let's generate nops up to size
    // limit
    auto GenRes = generateNopsToSizeLimit(IG, InstrGenCtx, CurrentStats);
    if (GenRes.Status != GenerationStatus::Ok)
      report_fatal_error("Nop generation during block fill by size failed",
                         false);
    CurrentStats.merge(GenerationStatistics(GenRes.Stats));
    return;
  }
  case GenerationStatus::Ok:
    return;
  }
}

template <typename T>
static void printDebugBrief(raw_ostream &OS, const Twine &Brief,
                            const T &Entity, int Indent = 0) {
  OS.indent(Indent) << Brief << ": ";
  Entity.print(OS);
  OS << "\n";
}

#define SNIPPY_DEBUG_BRIEF(...) LLVM_DEBUG(printDebugBrief(dbgs(), __VA_ARGS__))

void generate(planning::FunctionRequest &FunctionGenRequest,
              MachineFunction &MF, GeneratorContext &GC,
              SelfCheckInfo *SelfCheckInfo) {
  GenerationStatistics CurrMFGenStats;
  SNIPPY_DEBUG_BRIEF("request for function", FunctionGenRequest);
  auto &MLI = *GC.getMachineLoopInfo();
  std::set<MachineBasicBlock *, MIRComp> NotVisited;
  std::transform(MF.begin(), MF.end(),
                 std::inserter(NotVisited, NotVisited.begin()),
                 [](auto &MBB) { return &MBB; });
  auto *MBB = &MF.front();

  while (!NotVisited.empty()) {
    assert(MBB);
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.at(MBB);

    MachineLoop *ML = nullptr;
    // We need to know loops structure in selfcheck mode.
    if (GC.hasTrackingMode())
      ML = MLI.getLoopFor(MBB);

    if (ML && ML->getHeader() == MBB) {
      // Remember the current state (open transaction) before loop body
      // execution.
      assert(GC.hasTrackingMode());
      auto &I = GC.getOrCreateInterpreter();
      I.openTransaction();
    }

    if (ML && ML->getLoopLatch() == MBB) {
      // Loop latch is unconditional block that jumps to the loop header (by our
      // special canonicalization when selfcheck is enabled). As we expect that
      // each loop iteration will do the same, insert compensation code in the
      // latch block.
      assert(BBReq.isLimitReached(GenerationStatistics{}) &&
             "Latch block for compensation code cannot be requested to "
             "generate primary instructions");
      CurrMFGenStats.merge(generateCompensationCode(*MBB, GC));
    } else {
      if (GC.hasTrackingMode()) {
        if (snippy::interpretInstrs(MBB->begin(), MBB->getFirstTerminator(),
                                    GC) != GenerationStatus::Ok)
          report_fatal_error(
              "Interpretation failed on instructions inserted before "
              "main instructions generation routine.",
              false);
      }

      SNIPPY_DEBUG_BRIEF("request for reachable BasicBlock", BBReq);

      auto GeneratedStats = [&] {
        auto RP = GC.getRegisterPool();
        planning::InstructionGenerationContext InstrGenCtx{
            *MBB, MBB->getFirstTerminator(), &RP, GC, SelfCheckInfo};
        return snippy::generate(BBReq, InstrGenCtx);
      }();
      CurrMFGenStats.merge(std::move(GeneratedStats));
    }
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);

    if (GC.hasTrackingMode() && MBB == &MF.back()) {
      // The model has executed blocks from entry till exit. All other blocks
      // are dead and we'll handle then in the next loop below.
      break;
    }

    MBB = findNextBlock(MBB, NotVisited, ML, GC);
    if (!ML || ML->getExitBlock() != MBB)
      continue;

    auto &I = GC.getOrCreateInterpreter();
    // We are in exit block, it means that the loop was generated and we can
    // commit the transaction.
    I.commitTransaction();
    // Set expected values for registers that participate in the loop exit
    // condition (we execute only the first iteration of the loop). See
    // addIncomingValues/getIncomingValues description for additional details.
    for (const auto &[Reg, Value] : GC.getIncomingValues(MBB))
      I.setReg(Reg, Value);
  }

  LLVM_DEBUG(
      dbgs() << "Instructions generation with enabled tracking on model: "
             << NotVisited.size() << " basic block are dead.\n");

  assert((NotVisited.empty() || GC.hasTrackingMode()) &&
         "At this point some basic block might not be visited only when model "
         "controls generation routine.");

  if (GC.hasTrackingMode()) {
    // Model must not be used further as it finished execution.
    GC.disableTrackingMode();
    auto &I = GC.getOrCreateInterpreter();
    I.disableTransactionsTracking();
    I.resetMem();
  }

  MBB = findNextBlock(nullptr, NotVisited, nullptr, GC);
  while (!FunctionGenRequest.isLimitReached(CurrMFGenStats)) {
    assert(MBB);
    assert(!NotVisited.empty() &&
           "There are no more block to insert instructions.");
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.at(MBB);
    SNIPPY_DEBUG_BRIEF("request for dead BasicBlock", BBReq);

    auto GeneratedStats = [&] {
      auto RP = GC.getRegisterPool();
      planning::InstructionGenerationContext InstrGenCtx{
          *MBB, MBB->getFirstTerminator(), &RP, GC, SelfCheckInfo};
      return snippy::generate(BBReq, InstrGenCtx);
    }();
    CurrMFGenStats.merge(std::move(GeneratedStats));
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);
    MBB = findNextBlock(nullptr, NotVisited, nullptr, GC);
  }

  finalizeFunction(MF, FunctionGenRequest, CurrMFGenStats, GC, SelfCheckInfo);
}

void generate(planning::InstructionGroupRequest &IG,
              GenerationStatistics &CurrentStats,
              planning::InstructionGenerationContext &InstrGenCtx) {
  LLVM_DEBUG(dbgs() << "Generating IG:\n"; IG.print(dbgs(), 2););
  auto &GC = InstrGenCtx.GC;
  auto &MBB = InstrGenCtx.MBB;
  GenerationStatistics LocalStats{};
  if (GenerateInsertionPointHints)
    generateInsertionPointHint(InstrGenCtx);
  auto ItEnd = MBB.getFirstTerminator();
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  {
    auto RP = GC.getRegisterPool();
    auto *OldRegPool = InstrGenCtx.RP;
    InstrGenCtx.RP = &RP;
    auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    planning::InstrGroupGenerationRAIIWrapper InitAndFinish(IG, InstrGenCtx);
    planning::InstrRequestRange Range(IG, LocalStats);
    for (auto &&IR : Range) {
      if (GenerateInsertionPointHints && !IG.isInseparableBundle())
        generateInsertionPointHint(InstrGenCtx);
      generateInstruction(InstrInfo.get(IR.Opcode), InstrGenCtx,
                          std::move(IR.Preselected));
      switchPolicyIfNeeded(IG, IR.Opcode, MBB, GC);
      if (!IG.isInseparableBundle())
        ItBegin =
            processGeneratedInstructions(ItBegin, InstrGenCtx, IG, LocalStats);
    }
    InstrGenCtx.RP = OldRegPool;
  }
  // If instructions were not already postprocessed.
  if (IG.isInseparableBundle())
    processGeneratedInstructions(ItBegin, InstrGenCtx, IG, LocalStats);
  if (!IG.isLimitReached(LocalStats)) {
    auto &Ctx = GC.getLLVMState().getCtx();
    snippy::fatal(Ctx, "Generation failure",
                  "No more instructions to request but group limit still "
                  "hasn't been reached.");
  }
  CurrentStats.merge(LocalStats);
}

GenerationStatistics
generate(planning::BasicBlockRequest &BB,
         planning::InstructionGenerationContext &InstrGenCtx) {
  GenerationStatistics CurrentStats;
  for (auto &&IG : BB)
    generate(IG, CurrentStats, InstrGenCtx);
  return CurrentStats;
}
} // namespace snippy
} // namespace llvm
