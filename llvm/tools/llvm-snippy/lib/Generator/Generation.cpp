//===-- GenerationUtils.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Generation.h"
#include "snippy/Config/FPUSettings.h"
#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Generator/Backtrack.h"
#include "snippy/Generator/CallGraphState.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Generator/SnippyFunctionMetadata.h"
#include "snippy/Support/Options.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/Support/FormatVariadic.h"

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
static void switchPolicyIfNeeded(planning::InstructionGenerationContext &IGC,
                                 planning::InstructionGroupRequest &IG,
                                 unsigned Opcode) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &GenSettings = IGC.GenSettings;
  auto &MBB = IGC.MBB;
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  if (Tgt.needsGenerationPolicySwitch(Opcode))
    IG.changePolicy(planning::createGenPolicy(ProgCtx, GenSettings, MBB));
}

static void generateInsertionPointHint(
    planning::InstructionGenerationContext &InstrGenCtx) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  SnippyTgt.generateNop(InstrGenCtx);
}

enum class GenerationStatus {
  Ok,
  BacktrackingFailed,
  InterpretFailed,
  SizeFailed,
};

bool preInterpretBacktracking(const MachineInstr &MI, LLVMState &State,
                              Backtrack &BT) {
  if (!BT.isInstrValid(MI, State.getSnippyTarget()))
    return false;
  // TODO: maybe there will be another one way for back-tracking?
  return true;
}

template <typename InstrIt>
GenerationStatus interpretInstrs(InstrIt Begin, InstrIt End, LLVMState &State,
                                 const SimulatorContext &SimCtx) {
  auto &I = SimCtx.getInterpreter();
  assert(SimCtx.hasTrackingMode());

  for (auto &MI : iterator_range(Begin, End)) {
    if (SimCtx.BT && !preInterpretBacktracking(MI, State, *SimCtx.BT))
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
    InstrIt Begin, InstrIt End, SnippyProgramContext &ProgCtx,
    AsOneGenerator<bool, true, false> &SelfcheckPeriodTracker) {
  const auto &ST = ProgCtx.getLLVMState().getSnippyTarget();
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

void storeRefValue(InstructionGenerationContext &InstrGenCtx, MemAddr Addr,
                   APInt Val) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  ProgCtx.getLLVMState().getSnippyTarget().storeValueToAddr(InstrGenCtx, Addr,
                                                            Val);
}

void selfcheckOverflowGuard(const SectionDesc &SelfcheckSection,
                            unsigned long long SCAddress) {
  if (SCAddress >= SelfcheckSection.VMA + SelfcheckSection.Size)
    snippy::fatal("Selfcheck section overflow. Try to provide "
                  "\"selfcheck\" section description in layout file");
}
// Returns iterator to the first inserted instruction. So [returned It;
// InsertPos) contains inserted instructions.
// Register pool is taken by value as we'll do some local reservations on top
// of the existing register pool. All changes made must be canceled at the
// function exit.
SelfcheckIntermediateInfo<MachineBasicBlock::iterator>
storeRefAndActualValueForSelfcheck(
    Register DestReg, planning::InstructionGenerationContext &InstrGenCtx) {
  auto InsertPos = InstrGenCtx.Ins;
  auto &MBB = InstrGenCtx.MBB;
  auto &SimCtx = InstrGenCtx.SimCtx;
  assert(SimCtx.SCI);
  auto &SCAddress = SimCtx.SCI->CurrentAddress;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &ST = ProgCtx.getLLVMState().getSnippyTarget();
  auto &I = SimCtx.getInterpreter();
  auto RegValue = I.readReg(DestReg);

  bool IsBegin = MBB.begin() == InsertPos;
  auto FirstInserted = InsertPos;
  if (!IsBegin)
    FirstInserted = std::prev(InsertPos);

  ST.storeRegToAddr(InstrGenCtx, SCAddress, DestReg,
                    /* store the whole register */ 0);
  SelfcheckFirstStoreInfo<MachineBasicBlock::iterator> FirstStoreInfo{
      std::prev(InsertPos), SCAddress};
  SCAddress += ProgCtx.getSCStride();
  auto &SelfcheckSection = ProgCtx.getSelfcheckSection();
  selfcheckOverflowGuard(SelfcheckSection, SCAddress);
  storeRefValue(InstrGenCtx, SCAddress, RegValue);
  SCAddress += ProgCtx.getSCStride();
  selfcheckOverflowGuard(SelfcheckSection, SCAddress);


  if (IsBegin)
    return {MBB.begin(), FirstStoreInfo};
  return {std::next(FirstInserted), FirstStoreInfo};
}

struct GenerationResult {
  GenerationStatus Status;
  GenerationStatistics Stats;
};

static const auto &getAvailableFloatSemantics() {
  // (TODO): Some further work should be done to select the correct semantics
  // from MCRegister. For now snippy supports only IEEE formats.
  static const auto AvailableSemantics = std::array<const fltSemantics *, 3>{
      &APFloat::IEEEhalf(), &APFloat::IEEEsingle(), &APFloat::IEEEdouble()};
  return AvailableSemantics;
}

static Expected<const fltSemantics &>
getFloatSemanticsForReg(const MCRegisterInfo &RegInfo, MCRegister Reg) {
  auto RegClass = llvm::find_if(RegInfo.regclasses(),
                                [Reg](auto &C) { return C.contains(Reg); });
  if (RegClass == RegInfo.regclass_end())
    return createStringError(inconvertibleErrorCode(),
                             "Could not find register register class that "
                             "contains the requested register");
  auto RegWidth = RegClass->getSizeInBits();
  assert(RegWidth);
  auto &AvailableSemantics = getAvailableFloatSemantics();
  auto Found =
      llvm::find_if(AvailableSemantics, [&](const fltSemantics *Semantics) {
        return RegWidth == APFloat::getSizeInBits(*Semantics);
      });
  if (Found == AvailableSemantics.end())
    return createStringError(inconvertibleErrorCode(),
                             "No float semantics available for register class");
  auto *Sem = *Found;
  assert(Sem);
  return *Sem;
}

static void unNaNRegister(planning::InstructionGenerationContext &InstrGenCtx,
                          MCRegister Reg) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();

  auto &Cfg = InstrGenCtx.GenSettings.Cfg;
  assert(Cfg.FPUConfig);
  auto &OverwriteCfg = Cfg.FPUConfig->Overwrite;
  assert(OverwriteCfg);

  auto SelectedSemantics =
      getFloatSemanticsForReg(ProgCtx.getLLVMState().getRegInfo(), Reg);
  if (auto Err = SelectedSemantics.takeError())
    snippy::fatal(
        "Internal error",
        Twine("Cannot unNaNRegister: ").concat(toString(std::move(Err))));
  Expected<APInt> ValueToWriteOrErr =
      InstrGenCtx.getOrCreateFloatOverwriteValueSampler(*SelectedSemantics)
          .sample();

  if (auto Err = ValueToWriteOrErr.takeError())
    snippy::fatal(ProgCtx.getLLVMState().getCtx(), "Internal error",
                  std::move(Err));

  auto RP = InstrGenCtx.pushRegPool();
  Tgt.writeValueToReg(InstrGenCtx, *ValueToWriteOrErr, Reg);

  InstrGenCtx.PotentialNaNs.erase(Reg);
}

static bool isDestinationRegister(const MachineOperand &Op) {
  return Op.isReg() && Op.isDef();
}

static bool hasDestinationRegister(const MachineInstr &MI) {
  if (!MI.getNumDefs())
    return false;
  auto Operands = MI.operands();
  return llvm::find_if(Operands, isDestinationRegister) != Operands.end();
}

static MCRegister getFirstDestinationRegister(const MachineInstr &MI) {
  assert(MI.getNumDefs());
  auto Found = llvm::find_if(MI.operands(), isDestinationRegister);
  assert(Found != MI.operands().end() && "MI has no destination registers");
  return Found->getReg().asMCReg();
}

static MCRegister getDestinationRegister(const MachineInstr &MI) {
  assert(MI.getNumDefs() == 1);
  return getFirstDestinationRegister(MI);
}

static void markRegisterAsPotentialNaN(
    MCRegister Reg, planning::InstructionGenerationContext &InstrGenCtx) {
  InstrGenCtx.PotentialNaNs.insert(Reg);
}

static auto getInputRegisters(const MachineInstr &MI) {
  return llvm::make_filter_range(
      MI.operands(), [](auto &Op) { return Op.isReg() && Op.isUse(); });
}

static bool anyOfInputOperandsIsPotentiallyNaN(
    const MachineInstr &MI,
    const planning::InstructionGenerationContext &InstrGenCtx) {
  auto InputRegisters = getInputRegisters(MI);
  return llvm::any_of(InputRegisters, [&](auto &Op) {
    return InstrGenCtx.PotentialNaNs.count(Op.getReg().asMCReg());
  });
}

static bool allInputOperandsArePotentialNaNs(
    const MachineInstr &MI,
    const planning::InstructionGenerationContext &InstrGenCtx) {
  auto InputRegisters = getInputRegisters(MI);
  return llvm::all_of(InputRegisters, [&](auto &Op) {
    return InstrGenCtx.PotentialNaNs.count(Op.getReg().asMCReg());
  });
}

static bool hasFRegDestination(const MachineInstr &MI,
                               const SnippyTarget &Tgt) {
  if (!MI.getNumOperands())
    return false;
  return hasDestinationRegister(MI) &&
         Tgt.isFloatingPoint(getDestinationRegister(MI));
}

static bool shouldRewriteRegValue(
    const MachineInstr &MI,
    const planning::InstructionGenerationContext &InstrGenCtx) {
  auto &Cfg = InstrGenCtx.GenSettings.Cfg;
  if (!Cfg.FPUConfig)
    return false;
  auto &OverwriteCfg = Cfg.FPUConfig->Overwrite;
  assert(OverwriteCfg);
  switch (OverwriteCfg->Mode) {
  case FloatOverwriteMode::IF_ALL_OPERANDS:
    return allInputOperandsArePotentialNaNs(MI, InstrGenCtx);
  case FloatOverwriteMode::IF_ANY_OPERAND:
    return anyOfInputOperandsIsPotentiallyNaN(MI, InstrGenCtx);
  case FloatOverwriteMode::IF_MODEL_DETECTED_NAN: {
    auto &SimCtx = InstrGenCtx.SimCtx;
    assert(SimCtx.hasTrackingMode());
    auto Reg = getDestinationRegister(MI);
    auto &ProgCtx = InstrGenCtx.ProgCtx;
    auto SelectedSemantics =
        getFloatSemanticsForReg(ProgCtx.getLLVMState().getRegInfo(), Reg);
    if (auto Err = SelectedSemantics.takeError())
      snippy::fatal("Internal error", Twine("Cannot check if register is NaN: ")
                                          .concat(toString(std::move(Err))));
    auto &I = SimCtx.getInterpreter();
    APFloat Val(*SelectedSemantics, I.readReg(Reg));
    return Val.isNaN();
  }
  case FloatOverwriteMode::DISABLED:
    return false;
  }
  llvm_unreachable("Unknown float overwrite heuristic");
}

template <typename InstrIt>
void controlNaNPropagation(
    InstrIt Begin, InstrIt End,
    planning::InstructionGenerationContext &InstrGenCtx) {
  if (Begin == End)
    return;
  auto &SimCtx = InstrGenCtx.SimCtx;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &FPUConfig = InstrGenCtx.GenSettings.Cfg.FPUConfig;
  if (!FPUConfig || !FPUConfig->Overwrite)
    return;
  auto &Tgt = State.getSnippyTarget();
  auto Filtered =
      llvm::make_filter_range(llvm::make_range(Begin, End), [&](auto &MI) {
        return (MI.getNumDefs() == 1) && hasFRegDestination(MI, Tgt);
      });

  SmallVector<MCRegister, 8> RegsToUnNaN;
  llvm::for_each(Filtered, [&](auto &MI) {
    if (Tgt.canProduceNaN(MI.getDesc()) ||
        anyOfInputOperandsIsPotentiallyNaN(MI, InstrGenCtx))
      markRegisterAsPotentialNaN(getDestinationRegister(MI), InstrGenCtx);
    if (shouldRewriteRegValue(MI, InstrGenCtx))
      RegsToUnNaN.push_back(getDestinationRegister(MI));
  });
  auto Beg = std::prev(End);
  for (auto Reg : RegsToUnNaN)
    unNaNRegister(InstrGenCtx, Reg);
  if (!SimCtx.hasTrackingMode())
    return;
  if (interpretInstrs(++Beg, End, State, SimCtx) != GenerationStatus::Ok)
    snippy::fatal("Internal error",
                  "Failed to execute float overwrite on model");
}

template <typename InstrIt>
GenerationResult
handleGeneratedInstructions(InstrIt ItBegin,
                            planning::InstructionGenerationContext &InstrGenCtx,
                            const planning::RequestLimit &Limit) {
  auto &MBB = InstrGenCtx.MBB;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto ItEnd = InstrGenCtx.Ins;
  auto &SimCtx = InstrGenCtx.SimCtx;
  auto GeneratedCodeSize = State.getCodeBlockSize(ItBegin, ItEnd);
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
  if (sizeLimitIsExceeded(Limit, InstrGenCtx.Stats, GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);

  if (!SimCtx.hasTrackingMode()) {
    controlNaNPropagation(ItBegin, ItEnd, InstrGenCtx);
    return ReportGenerationResult(GenerationStatus::Ok);
  }

  auto InterpretInstrsResult =
      interpretInstrs(ItBegin, ItEnd, ProgCtx.getLLVMState(), SimCtx);
  if (InterpretInstrsResult != GenerationStatus::Ok)
    return ReportGenerationResult(InterpretInstrsResult);

  controlNaNPropagation(ItBegin, ItEnd, InstrGenCtx);

  if (!SimCtx.SCI)
    return ReportGenerationResult(GenerationStatus::Ok);

  if (PrimaryInstrCount == 0) {
    LLVM_DEBUG(
        dbgs() << "Ignoring selfcheck for the supportive instruction\n";);
    return ReportGenerationResult(GenerationStatus::Ok);
  }
  assert(PrimaryInstrCount >= 1);
  assert(!InstrGenCtx.Stats.UnableToFitAnymore &&
         "There is no room left for selfcheck");
  assert(SimCtx.SCI);

  // Collect instructions that can and should be selfchecked.
  auto SelfcheckCandidates = collectSelfcheckCandidates(
      ItBegin, ItEnd, ProgCtx, SimCtx.SCI->PeriodTracker);

  // Collect def registers from the candidates.
  // Use SetVector as we want to preserve order of insertion.
  SetVector<SelfcheckAnnotationInfo<InstrIt>> Defs;
  const auto &ST = State.getSnippyTarget();
  // Reverse traversal as we need to sort definitions by their last use.
  for (auto Inst : reverse(SelfcheckCandidates)) {
    // TODO: Add self-check for instructions without definitions
    if (Inst->getDesc().getNumDefs() == 0)
      continue;
    auto Regs = ST.getRegsForSelfcheck(*Inst, InstrGenCtx);
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
  auto RP = InstrGenCtx.pushRegPool();
  std::vector<SelfcheckFirstStoreInfo<InstrIt>> StoresInfo;
  for (const auto &Def : Defs) {
    llvm::for_each(ST.getPhysRegsFromUnit(Def.DestReg, State.getRegInfo()),
                   [&RP](auto SimpleReg) { RP->addReserved(SimpleReg); });
    // This is done for backward compatibility.
    auto TmpRP = InstrGenCtx.pushRegPool();
    auto SelfcheckInterInfo =
        storeRefAndActualValueForSelfcheck(Def.DestReg, InstrGenCtx);
    InsPoint = InstrGenCtx.Ins = SelfcheckInterInfo.NextInsertPos;
    // Collect information about first generated selfcehck store for a primary
    // instr
    StoresInfo.push_back(SelfcheckInterInfo.FirstStoreInfo);
  }
  InstrGenCtx.Ins = ItEnd;

  assert(Defs.size() == StoresInfo.size());
  // Add collected information about generated selfcheck stores for selfcheck
  // annotation
  for (const auto &[Def, StoreInfo] : zip(Defs, StoresInfo))
    InstrGenCtx.getSnippyModule()
        .getOrAddResult<SelfCheckMap>()
        .addToSelfcheckMap(
            StoreInfo.Address,
            std::distance(Def.Inst, std::next(StoreInfo.FirstStoreInstrPos)));
  // Execute self-check instructions
  auto &I = SimCtx.getInterpreter();
  if (!I.executeChainOfInstrs(State, InsPoint, ItEnd))
    I.reportSimulationFatalError(
        "Failed to execute chain of instructions in tracking mode");

  // Check size requirements after selfcheck addition.
  GeneratedCodeSize = State.getCodeBlockSize(ItBegin, ItEnd);
  if (sizeLimitIsExceeded(Limit, InstrGenCtx.Stats, GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  return ReportGenerationResult(GenerationStatus::Ok);
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
    InstructionGenerationContext &InstrGenCtx, const MCInstrDesc &InstrDesc,
    const MCOperandInfo &MCOpInfo,
    const planning::PreselectedOpInfo &Preselected, unsigned OpIndex,
    ArrayRef<MachineOperand> PregeneratedOperands) {
  auto OpType = MCOpInfo.OperandType;

  auto &RP = InstrGenCtx.getRegPool();
  auto &MBB = InstrGenCtx.MBB;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &State = ProgCtx.getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RegGen = ProgCtx.getRegGen();
  auto &GenSettings = InstrGenCtx.GenSettings;
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
      auto RegClass =
          SnippyTgt.getRegClass(InstrGenCtx, OperandRegClassID, OpIndex,
                                InstrDesc.getOpcode(), RegInfo);
      auto Exclude = SnippyTgt.excludeRegsForOperand(InstrGenCtx, RegClass,
                                                     InstrDesc, OpIndex);
      auto Include = SnippyTgt.includeRegs(RegClass);
      AccessMaskBit Mask = IsDst ? AccessMaskBit::W : AccessMaskBit::R;

      auto CustomMask =
          SnippyTgt.getCustomAccessMaskForOperand(InstrDesc, OpIndex);
      // TODO: add checks that target-specific and generic restrictions are
      // not conflicting.
      if (CustomMask != AccessMaskBit::None)
        Mask = CustomMask;
      auto RegOpt = RegGen.generate(RegClass, OperandRegClassID, RegInfo, RP,
                                    MBB, SnippyTgt, Exclude, Include, Mask);
      if (!RegOpt)
        return std::nullopt;
      Reg = RegOpt.value();
      if (SnippyTgt.isPhysRegClass(OperandRegClassID, RegInfo))
        Reg = SnippyTgt.getFirstPhysReg(Reg, RegInfo);
    }
    SnippyTgt.reserveRegsIfNeeded(InstrGenCtx, InstrDesc.getOpcode(),
                                  /* isDst */ IsDst,
                                  /* isMem */ false, Reg);
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
      if (SnippyTgt.isPhysRegClass(OperandRegClassID, RegInfo))
        Reg = SnippyTgt.getFirstPhysReg(Reg, RegInfo);
    }
    // FIXME: RW mask is too restrictive for the majority of instructions.
    SnippyTgt.reserveRegsIfNeeded(InstrGenCtx, InstrDesc.getOpcode(),
                                  /* isDst */ Preselected.getFlags() &
                                      RegState::Define,
                                  /* isMem */ true, Reg);
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
    return SnippyTgt.generateTargetOperand(
        ProgCtx, GenSettings, InstrDesc.getOpcode(), OpType, StridedImm);
  }
  llvm_unreachable("this operand type unsupported");
}
std::optional<SmallVector<MachineOperand, 8>> tryToPregenerateOperands(
    InstructionGenerationContext &InstrGenCtx, const MCInstrDesc &InstrDesc,
    const std::vector<planning::PreselectedOpInfo> &Preselected) {
  SmallVector<MachineOperand, 8> PregeneratedOperands;
  assert(InstrDesc.getNumOperands() == Preselected.size());
  iota_range<unsigned long> PreIota(0, Preselected.size(),
                                    /* Inclusive */ false);
  for (const auto &[MCOpInfo, PreselOpInfo, Index] :
       zip(InstrDesc.operands(), Preselected, PreIota)) {
    auto OpOpt =
        pregenerateOneOperand(InstrGenCtx, InstrDesc, MCOpInfo, PreselOpInfo,
                              Index, PregeneratedOperands);
    if (!OpOpt)
      return std::nullopt;
    PregeneratedOperands.push_back(OpOpt.value());
  }
  return PregeneratedOperands;
}

SmallVector<MachineOperand, 8> pregenerateOperands(
    InstructionGenerationContext &InstrGenCtx, const MCInstrDesc &InstrDesc,
    const std::vector<planning::PreselectedOpInfo> &Preselected) {
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &RegGenerator = ProgCtx.getRegGen();
  auto &State = ProgCtx.getLLVMState();
  auto &InstrInfo = State.getInstrInfo();
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();

  auto IterNum = 0u;
  constexpr auto FailsMaxNum = 10000u;

  while (IterNum < FailsMaxNum) {
    RP.reset();
    // Initialized registers must not be overwritten during other instruction
    // preparation.
    llvm::for_each(Preselected, [&](const auto &Op) {
      if (!Op.isReg())
        return;
      llvm::for_each(Tgt.getPhysRegsFromUnit(Op.getReg(), RI),
                     [&RP](auto SimpleReg) {
                       RP.addReserved(SimpleReg, AccessMaskBit::W);
                     });
    });
    RegGenerator.setRegContextForPlugin();
    auto PregeneratedOperandsOpt =
        tryToPregenerateOperands(InstrGenCtx, InstrDesc, Preselected);
    IterNum++;

    if (PregeneratedOperandsOpt)
      return std::move(PregeneratedOperandsOpt.value());
  }

  snippy::fatal(formatv("Limit on failed generation attempts exceeded "
                        "on instruction {0}",
                        InstrInfo.getName(InstrDesc.getOpcode())));
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

static GenerationResult
generateNopsToSizeLimit(const planning::RequestLimit &Limit,
                        planning::InstructionGenerationContext &InstrGenCtx) {
  assert(Limit.isSizeLimit());

  auto &MBB = InstrGenCtx.MBB;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto ItBegin = InstrGenCtx.Ins == MBB.begin() ? InstrGenCtx.Ins
                                                : std::prev(InstrGenCtx.Ins);

  // assume nop size is the lowest possible instruction size
  size_t NopSize =
      *SnpTgt.getPossibleInstrsSize(InstrGenCtx.getSubtargetImpl()).begin();
  size_t GeneratedNopsSize = 0;

  while (Limit.getSizeLeft(InstrGenCtx.Stats) >=
         (GeneratedNopsSize + NopSize)) {
    auto *MI = SnpTgt.generateNop(InstrGenCtx);
    assert(MI && "Nop generation failed");

    assert(NopSize == SnpTgt.getInstrSize(*MI, State));
    GeneratedNopsSize += NopSize;
  }

  InstrGenCtx.Stats.UnableToFitAnymore = true;

  if (ItBegin != std::prev(InstrGenCtx.Ins)) {
    ItBegin++;
  }
  return handleGeneratedInstructions(ItBegin, InstrGenCtx, Limit);
}

bool sizeLimitIsExceeded(const planning::RequestLimit &Lim,
                         const GenerationStatistics &CommitedStats,
                         size_t NewGeneratedCodeSize) {
  if (!Lim.isSizeLimit())
    return false;
  return NewGeneratedCodeSize > Lim.getSizeLeft(CommitedStats);
}

AddressGenInfo chooseAddrGenInfoForInstrCallback(
    LLVMContext &Ctx,
    std::optional<SnippyLoopInfo::LoopGenerationInfo> CurLoopGenInfo,
    size_t AccessSize, size_t Alignment, const MemoryAccess &MemoryScheme) {
  (void)CurLoopGenInfo; // for future extensibility
  // AddressGenInfo for one element access.
  return AddressGenInfo::singleAccess(AccessSize, Alignment, false /* Burst */);
}

static AccessSampleResult
chooseAddrInfoForInstr(MachineInstr &MI, InstructionGenerationContext &IGC,
                       const MachineLoop *ML, const SnippyLoopInfo *SLI) {
  assert(!ML || SLI);
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &MS = IGC.getMemoryAccessSampler();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto Opcode = MI.getDesc().getOpcode();

  auto [AccessSize, Alignment] =
      SnippyTgt.getAccessSizeAndAlignment(ProgCtx, Opcode, *MI.getParent());

  auto CurLoopGenInfo =
      ML ? SLI->getLoopsGenerationInfoForMBB(ML->getHeader()) : std::nullopt;

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

  auto Result = MS.sample(AccessSize, Alignment, ChooseAddrGenInfo);
  if (auto Err = Result.takeError()) {
    std::string InstrStr;
    raw_string_ostream OS(InstrStr);
    MI.print(OS);
    snippy::fatal(formatv("Cannot sample memory access for instruction \"{0}\"",
                          StringRef(InstrStr).rtrim()),
                  Twine("\n").concat(toString(std::move(Err))));
  }
  const auto &AddrInfo = Result->AddrInfo;

  assert(AddrInfo.MaxOffset >= 0);
  assert(AddrInfo.MinOffset <= 0);

  return *Result;
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

unsigned chooseAddressRegister(InstructionGenerationContext &IGC,
                               MachineInstr &MI, const AddressPart &AP) {
  auto &RP = IGC.getRegPool();
  auto &SimCtx = IGC.SimCtx;
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
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
  auto &I = SimCtx.getInterpreter();
  auto AddrValue = AP.Value;

  auto GetMatCost = [&](auto Reg) {
    return SnippyTgt.getTransformSequenceLength(IGC, I.readReg(Reg), AddrValue,
                                                Reg);
  };
  auto ChosenRegIt =
      std::min_element(PickedRegisters.begin(), PickedRegisters.end(),
                       [&GetMatCost](auto &Lhs, auto &Rhs) {
                         return GetMatCost(Lhs) < GetMatCost(Rhs);
                       });
  assert(ChosenRegIt != PickedRegisters.end());
  auto ChosenReg = *ChosenRegIt;

  // Update corresponding MI operands with picked register.
  for (auto &OpIdx : AP.operands) {
    auto &Op = MI.getOperand(OpIdx);
    assert(Op.isReg() && "Operand is not register");
    Op.setReg(ChosenReg);
  }

  return ChosenReg;
}
void postprocessMemoryOperands(MachineInstr &MI,
                               planning::InstructionGenerationContext &IGC) {
  auto *MAI = IGC.MAI;
  auto *MLI = IGC.MLI;
  auto &RP = IGC.getRegPool();
  const auto *SLI = IGC.SLI;
  auto &SimCtx = IGC.SimCtx;
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto Opcode = MI.getDesc().getOpcode();
  auto NumAddrsToGen = SnippyTgt.countAddrsToGenerate(Opcode);
  auto *MBB = MI.getParent();
  auto *ML = MLI ? MLI->getLoopFor(MBB) : nullptr;
  assert(!ML || SLI);

  for (size_t i = 0; i < NumAddrsToGen; ++i) {
    auto [AddrInfo, AddrGenInfo] = chooseAddrInfoForInstr(MI, IGC, ML, SLI);
    bool GenerateStridedLoad = !AddrGenInfo.isSingleElement();
    auto AccessSize = AddrInfo.AccessSize;

    if (GenerateStridedLoad && SimCtx.hasTrackingMode())
      snippy::fatal(State.getCtx(), "Incompatible features",
                    "Can't use generate strided accesses in loops with "
                    "tracking mode enabled");

    auto &&[RegToValue, ChosenAddresses] =
        SnippyTgt.breakDownAddr(IGC, AddrInfo, MI, i);

    for (unsigned j = 0; j < AddrGenInfo.NumElements; ++j) {
      if (MAI)
        addMemAccessToDump(ChosenAddresses, *MAI, AccessSize);

      for_each(ChosenAddresses,
               [Stride = AddrInfo.MinStride](auto &Val) { Val += Stride; });
    }

    auto OldIns = IGC.Ins;
    IGC.Ins = MI.getIterator();
    for (auto &AP : RegToValue) {
      MCRegister Reg = AP.FixedReg;
      if (SimCtx.hasTrackingMode() &&
          IGC.GenSettings.TrackingConfig.AddressVH) {
        auto &I = SimCtx.getInterpreter();
        Reg = chooseAddressRegister(IGC, MI, AP);
        SnippyTgt.transformValueInReg(IGC, I.readReg(Reg), AP.Value, Reg);
      } else {
        SnippyTgt.writeValueToReg(IGC, AP.Value, Reg);
      }
      // Address registers must not be overwritten during other memory operands
      // preparation.
      RP.addReserved(Reg, AccessMaskBit::W);
    }
    IGC.Ins = OldIns;
  }
}

static void reloadGlobalRegsFromMemory(InstructionGenerationContext &IGC) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto &SpilledToMem = IGC.GenSettings.RegistersConfig.SpilledToMem;
  if (SpilledToMem.empty())
    return;
  assert(ProgCtx.hasProgramStateSaveSpace());
  auto &SaveLocs = ProgCtx.getProgramStateSaveSpace();
  for (auto &Reg : SpilledToMem) {
    auto &Addr = SaveLocs.getSaveLocation(Reg);
    Tgt.generateSpillToAddr(IGC, Reg, Addr.Local);
  }
  for (auto &Reg : SpilledToMem) {
    auto &Addr = SaveLocs.getSaveLocation(Reg);
    Tgt.generateReloadFromAddr(IGC, Reg, Addr.Global);
  }
}

static void reloadLocallySpilledRegs(InstructionGenerationContext &IGC) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto &SpilledToMem = IGC.GenSettings.RegistersConfig.SpilledToMem;
  if (SpilledToMem.empty())
    return;
  assert(ProgCtx.hasProgramStateSaveSpace());
  auto &SaveLocs = ProgCtx.getProgramStateSaveSpace();
  for (auto &Reg : SpilledToMem) {
    auto &Addr = SaveLocs.getSaveLocation(Reg);
    Tgt.generateReloadFromAddr(IGC, Reg, Addr.Local);
  }
}

MachineInstr *
generateCall(unsigned OpCode,
             planning::InstructionGenerationContext &InstrGenCtx) {
  auto &MBB = InstrGenCtx.MBB;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &GenSettings = InstrGenCtx.GenSettings;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  assert(InstrGenCtx.CGS);
  auto &CGS = *InstrGenCtx.CGS;
  auto *Node = CGS.getNode(&(MBB.getParent()->getFunction()));
  auto CalleeCount = Node->callees().size();
  if (!CalleeCount)
    return nullptr;
  auto CalleeIdx = RandEngine::genInRangeExclusive(CalleeCount);
  auto *CalleeNode = std::next(Node->callees().begin(), CalleeIdx)->Dest;
  auto FunctionIdx =
      RandEngine::genInRangeExclusive(CalleeNode->functions().size());

  auto &CallTarget = *(CalleeNode->functions()[FunctionIdx]);
  assert(CallTarget.hasName());
  if (CalleeNode->isExternal() && GenSettings.Cfg.hasSectionToSpillGlobalRegs())
    reloadGlobalRegsFromMemory(InstrGenCtx);

  auto TargetStackPointer = SnippyTgt.getStackPointer();
  auto RealStackPointer = ProgCtx.getStackPointer();

  // If we redefined stack pointer register, before generating external function
  // call we need to copy stack pointer value to target default stack pointer
  // and do reverse after returning from external call
  if (CalleeNode->isExternal() && (RealStackPointer != TargetStackPointer))
    SnippyTgt.copyRegToReg(InstrGenCtx, RealStackPointer, TargetStackPointer);

  auto *Call = SnippyTgt.generateCall(InstrGenCtx, CallTarget,
                                      /* AsSupport */ false, OpCode);

  if (!GenSettings.isRegSpilledToMem(RealStackPointer) &&
      CalleeNode->isExternal() && (RealStackPointer != TargetStackPointer))
    SnippyTgt.copyRegToReg(InstrGenCtx, TargetStackPointer, RealStackPointer);

  if (CalleeNode->isExternal() && GenSettings.Cfg.hasSectionToSpillGlobalRegs())
    reloadLocallySpilledRegs(InstrGenCtx);

  Node->markAsCommitted(CalleeNode);
  return Call;
}

static bool
isPostprocessNeeded(const MCInstrDesc &InstrDesc,
                    const std::vector<planning::PreselectedOpInfo> &Preselected,
                    const GeneratorSettings &GenSettings) {
  // 1. If any information about operands is provided from the caller, we won't
  //    do any postprocessing.
  if (!GenSettings.isApplyValuegramEachInstr())
    return Preselected.empty();
  // 2. In the case of the option -valuegram-operands-regs, we do partial
  //    initialization in the case of memory instructions. We initialize only
  //    those registers that are not memory addresses and still need a
  //    postprocess to specify addresses.
  // 3. In case -riscv-init-fregs-from-memory we initialize all memory
  //    operands and do not need a postprocess.
  return llvm::any_of(Preselected, [](const auto &Op) { return Op.isUnset(); });
}

MachineInstr *
randomInstruction(const MCInstrDesc &InstrDesc,
                  std::vector<planning::PreselectedOpInfo> Preselected,
                  planning::InstructionGenerationContext &InstrGenCtx) {
  auto &MBB = InstrGenCtx.MBB;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &GenSettings = InstrGenCtx.GenSettings;
  const auto &SnippyTgt = State.getSnippyTarget();

  auto MIB = getMainInstBuilder(SnippyTgt, MBB, InstrGenCtx.Ins,
                                MBB.getParent()->getFunction().getContext(),
                                InstrDesc);

  bool DoPostprocess = isPostprocessNeeded(InstrDesc, Preselected, GenSettings);

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

  auto RP = InstrGenCtx.pushRegPool();
  auto PregeneratedOperands =
      pregenerateOperands(InstrGenCtx, InstrDesc, Preselected);
  for (auto &Op : PregeneratedOperands)
    MIB.add(Op);

  if (DoPostprocess)
    postprocessMemoryOperands(*MIB, InstrGenCtx);
  // FIXME:
  // We have a lot of problems with rollback and configurations
  // After this, we can have additional instruction after main one!
  [[maybe_unused]] auto PostProcessInstPos = std::next(MIB->getIterator());
  assert(PostProcessInstPos == InstrGenCtx.Ins);
  SnippyTgt.instructionPostProcess(InstrGenCtx, *MIB);
  return MIB;
}

void spillPseudoInstImplicitReg(MachineInstr &MI, Register Reg,
                                InstructionGenerationContext &IGC) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &SnpTgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto *MBBPtr = MI.getParent();
  assert(MBBPtr);
  auto RealStackPointer = ProgCtx.getStackPointer();

  auto OldIns = IGC.Ins;

  // FIXME: This code can be unsuitable for some platforms
  auto SpillPoint = MachineBasicBlock::iterator(MI);
  IGC.Ins = SpillPoint;
  SnpTgt.generateSpillToStack(IGC, Reg, RealStackPointer);

  auto ReloadPoint = std::next(MachineBasicBlock::iterator(MI));
  IGC.Ins = ReloadPoint;
  SnpTgt.generateReloadFromStack(IGC, Reg, RealStackPointer);
  IGC.Ins = OldIns;
}

void spillPseudoInstImplicitRegs(
    MachineInstr &MI, planning::InstructionGenerationContext &InstrGenCtx) {
  auto &&ImplicitRegsOps = make_filter_range(
      MI.operands(), [](auto &&Op) { return Op.isReg() && Op.isImplicit(); });

  for (auto &Op : ImplicitRegsOps) {
    auto Reg = Op.getReg();
    if (InstrGenCtx.getRegPool().isReserved(Reg, *MI.getParent()))
      spillPseudoInstImplicitReg(MI, Reg, InstrGenCtx);
  }
}

void generateRealInstruction(
    const MCInstrDesc &InstrDesc,
    planning::InstructionGenerationContext &InstrGenCtx,
    std::vector<planning::PreselectedOpInfo> Preselected) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto Opc = InstrDesc.getOpcode();
  const auto &SnippyTgt = State.getSnippyTarget();

  assert(!InstrDesc.isBranch() &&
         "All branch instructions expected to be generated separately");

  if (SnippyTgt.requiresCustomGeneration(InstrDesc)) {
    SnippyTgt.generateCustomInst(InstrDesc, InstrGenCtx);
    return;
  }

  auto *MI =
      (SnippyTgt.isCall(Opc))
          ? generateCall(Opc, InstrGenCtx)
          : randomInstruction(InstrDesc, std::move(Preselected), InstrGenCtx);

  if (!MI || !MI->isPseudo())
    return;

  spillPseudoInstImplicitRegs(*MI, InstrGenCtx);
}

void generateInstruction(const MCInstrDesc &InstrDesc,
                         planning::InstructionGenerationContext &InstrGenCtx,
                         std::vector<planning::PreselectedOpInfo> Preselected) {

  generateRealInstruction(InstrDesc, InstrGenCtx, std::move(Preselected));
}

MachineBasicBlock *findNextBlockOnModel(MachineBasicBlock &MBB,
                                        LLVMState &State,
                                        const SimulatorContext &SimCtx) {
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &I = SimCtx.getInterpreter();
  auto PC = I.getPC();
  for (auto &Branch : MBB.terminators()) {
    assert(Branch.isBranch());
    if (Branch.isUnconditionalBranch())
      return SnippyTgt.getBranchDestination(Branch);

    assert(!Branch.isIndirectBranch() &&
           "Indirect branches are not supported for execution on the model "
           "during code generation.");
    assert(Branch.isConditionalBranch());
    I.addInstr(Branch, State);

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
              const MachineLoop *ML, LLVMState &State,
              const SimulatorContext &SimCtx) {
  if (SimCtx.hasTrackingMode() && MBB) {
    // In selfcheck/backtracking mode we go to exit block of the loop right
    // after the latch block.
    if (ML && ML->getLoopLatch() == MBB)
      return ML->getExitBlock();
    return findNextBlockOnModel(*MBB, State, SimCtx);
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
  auto &ProgCtx = GC.getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  InstructionGenerationContext IGC{MBB, MBB.getFirstTerminator(), GC};
  auto RP = IGC.pushRegPool();
  for (auto &&[RegIdx, Value] : RegsSnapshot) {
    auto Reg = SnippyTgt.regIndexToMCReg(IGC, RegIdx, Storage);
    // FIXME: we expect that writeValueToReg won't corrupt other registers of
    // the same class even if they were not reserved. Also we expect that
    // writing to FP/V register may use only non-reserved GPR registers.
    SnippyTgt.writeValueToReg(
        IGC, toAPInt(Value, SnippyTgt.getRegBitWidth(Reg, IGC)), Reg);
  }
}

GenerationStatistics generateCompensationCode(MachineBasicBlock &MBB,
                                              GeneratorContext &GC,
                                              const SimulatorContext &SimCtx) {
  assert(SimCtx.hasTrackingMode());

  InstructionGenerationContext IGC{MBB, MBB.getFirstTerminator(), GC};
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto &I = SimCtx.getInterpreter();
  auto MemSnapshot = I.getMemBeforeTransaction();
  auto FRegsSnapshot = I.getFRegsBeforeTransaction();
  auto VRegsSnapshot = I.getVRegsBeforeTransaction();
  // Restoring memory requires GPR registers to prepare address and value to
  // write, so we need know which registers were changed. We cannot do it in the
  // current opened transaction as we'll mess up exiting from the loop state.
  // So, open a new one.
  I.openTransaction();
  if (!MemSnapshot.empty()) {
    auto StackSec = ProgCtx.getStackSection();
    auto SelfcheckSec = ProgCtx.getSelfcheckSection();
    for (auto [Addr, Data] : MemSnapshot) {
      assert(StackSec.Size > 0 && "Stack section cannot have zero size");
      // Skip stack and selfcheck memory. None of them can be rolled back.
      if (std::clamp<size_t>(Addr, StackSec.VMA,
                             StackSec.VMA + StackSec.Size) == Addr ||
          std::clamp<size_t>(Addr, SelfcheckSec.VMA,
                             SelfcheckSec.VMA + SelfcheckSec.Size) == Addr)
        continue;
      auto RP = IGC.pushRegPool();
      SnippyTgt.storeValueToAddr(
          IGC, Addr,
          {sizeof(Data) * CHAR_BIT, static_cast<unsigned char>(Data)});
    }
  }

  writeRegsSnapshot(FRegsSnapshot, MBB, RegStorageType::FReg, GC);
  writeRegsSnapshot(VRegsSnapshot, MBB, RegStorageType::VReg, GC);
  // Execute inserted instructions to get a change made by the opened
  // transaction.
  if (snippy::interpretInstrs(MBB.begin(), MBB.getFirstTerminator(), State,
                              SimCtx) != GenerationStatus::Ok)
    snippy::fatal("Inserted compensation code is incorrect");

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
      0, State.getCodeBlockSize(MBB.begin(), MBB.getFirstTerminator()));
}

void finalizeFunction(MachineFunction &MF, planning::FunctionRequest &Request,
                      const GenerationStatistics &MFStats, GeneratorContext &GC,
                      const SimulatorContext &SimCtx, const CallGraphState &CGS,
                      MemAccessInfo *MAI) {
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &MBB = MF.back();

  auto LastInstr = GC.getGenSettings().getLastInstr();
  bool NopLastInstr = LastInstr.empty();
  planning::InstructionGenerationContext InstrGenCtx{MBB, MF.back().end(), GC,
                                                     SimCtx};
  auto RP = InstrGenCtx.pushRegPool();

  // Secondary functions always return.
  if (!CGS.isRootFunction(MF)) {
    State.getSnippyTarget().generateReturn(InstrGenCtx);
    return;
  }

  // Root functions are connected via tail calls.
  if (!CGS.isExitFunction(MF)) {
    State.getSnippyTarget().generateTailCall(InstrGenCtx,
                                             *CGS.nextRootFunction(MF));
    return;
  }

  auto *ExitSym =
      MF.getContext().getOrCreateSymbol(Linker::getExitSymbolName());

  // User may ask for last instruction to be return.
  if (GC.getGenSettings().useRetAsLastInstr()) {
    State.getSnippyTarget()
        .generateReturn(InstrGenCtx)
        ->setPreInstrSymbol(MF, ExitSym);
    return;
  }

  // Or to generate nop
  if (NopLastInstr) {
    State.getSnippyTarget().generateNop(InstrGenCtx);
    MBB.back().setPostInstrSymbol(MF, ExitSym);
    return;
  }
  for (auto &&FinalReq : Request.getFinalGenReqs(MFStats)) {
    assert((!FinalReq.limit().isNumLimit() || FinalReq.limit().getLimit()) &&
           "FinalReq is empty!");
    InstrGenCtx.append(MAI);
    snippy::generate(FinalReq, InstrGenCtx);
  }

  // Mark last generated instruction as support one.
  setAsSupportInstr(*(--MBB.getFirstTerminator()), State.getCtx());
  MBB.back().setPreInstrSymbol(MF, ExitSym);
}

void processGenerationResult(
    const planning::RequestLimit &Limit,
    planning::InstructionGenerationContext &InstrGenCtx,
    const GenerationResult &IntRes) {
  LLVM_DEBUG(printInterpretResult(dbgs(), "      ", IntRes); dbgs() << "\n");
  auto &SimCtx = InstrGenCtx.SimCtx;
  auto &BacktrackCount = InstrGenCtx.BacktrackCount;
  auto &SizeErrorCount = InstrGenCtx.SizeErrorCount;
  switch (IntRes.Status) {
  case GenerationStatus::InterpretFailed: {
    SimCtx.getInterpreter().reportSimulationFatalError(
        "Fail to execute generated instruction in tracking mode\n");
  }
  case GenerationStatus::BacktrackingFailed: {
    ++BacktrackCount;
    if (BacktrackCount < BTThreshold)
      return;
    snippy::fatal("Back-tracking events threshold is exceeded");
  }
  case GenerationStatus::SizeFailed: {
    ++SizeErrorCount;
    if (SizeErrorCount < SizeErrorThreshold)
      return;

    // We have size error too many times, let's generate nops up to size
    // limit
    auto GenRes = generateNopsToSizeLimit(Limit, InstrGenCtx);
    if (GenRes.Status != GenerationStatus::Ok)
      snippy::fatal("Nop generation during block fill by size failed");
    InstrGenCtx.Stats.merge(GenRes.Stats);
    return;
  }
  case GenerationStatus::Ok:
    return;
  }
}

MachineBasicBlock::iterator processGeneratedInstructions(
    MachineBasicBlock::iterator ItBegin,
    planning::InstructionGenerationContext &InstrGenCtx,
    const planning::RequestLimit &Limit) {
  auto &MBB = InstrGenCtx.MBB;
  auto ItEnd = InstrGenCtx.Ins;
  ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
  auto IntRes = handleGeneratedInstructions(ItBegin, InstrGenCtx, Limit);
  processGenerationResult(Limit, InstrGenCtx, IntRes);
  InstrGenCtx.Stats.merge(IntRes.Stats);
  return std::prev(ItEnd);
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
              const SimulatorContext &SimCtx, MachineLoopInfo *MLI,
              const CallGraphState &CGS, MemAccessInfo *MAI,
              const SnippyLoopInfo *SLI, SnippyFunctionMetadata *SFM) {
  GenerationStatistics CurrMFGenStats;
  SNIPPY_DEBUG_BRIEF("request for function", FunctionGenRequest);
  std::set<MachineBasicBlock *, MIRComp> NotVisited;
  std::transform(MF.begin(), MF.end(),
                 std::inserter(NotVisited, NotVisited.begin()),
                 [](auto &MBB) { return &MBB; });
  auto *MBB = &MF.front();
  auto &ProgCtx = GC.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  if (SimCtx.hasTrackingMode()) {
    // Explicitly prepare interpreter PC.
    assert(SimCtx.Runner);
    auto &I = SimCtx.Runner->getPrimaryInterpreter();
    auto StartPC = GC.getProgramContext().getLinker().getStartPC();
    assert(StartPC);
    I.setPC(*StartPC);
  }
  while (!NotVisited.empty()) {
    assert(MBB);
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.at(MBB);

    MachineLoop *ML = nullptr;
    // We need to know loops structure in selfcheck mode.
    if (SimCtx.hasTrackingMode()) {
      assert(MLI && "Machine Loop Info must be available here");
      ML = MLI->getLoopFor(MBB);
    }

    if (ML && ML->getHeader() == MBB) {
      // Remember the current state (open transaction) before loop body
      // execution.
      assert(SimCtx.hasTrackingMode());
      auto &I = SimCtx.getInterpreter();
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
      CurrMFGenStats.merge(generateCompensationCode(*MBB, GC, SimCtx));
    } else {
      SNIPPY_DEBUG_BRIEF("request for reachable BasicBlock", BBReq);

      planning::InstructionGenerationContext InstrGenCtx{*MBB, MBB->begin(), GC,
                                                         SimCtx};
      auto RP = InstrGenCtx.pushRegPool();
      InstrGenCtx.append(MLI).append(&CGS).append(MAI).append(SLI).append(SFM);
      snippy::generate(BBReq, InstrGenCtx);
      CurrMFGenStats.merge(InstrGenCtx.Stats);
    }
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);

    if (SimCtx.hasTrackingMode() && MBB == &MF.back()) {
      // The model has executed blocks from entry till exit. All other blocks
      // are dead and we'll handle then in the next loop below.
      break;
    }

    MBB = findNextBlock(MBB, NotVisited, ML, State, SimCtx);
    if (!ML || ML->getExitBlock() != MBB)
      continue;

    auto &I = SimCtx.getInterpreter();
    // We are in exit block, it means that the loop was generated and we can
    // commit the transaction.
    I.commitTransaction();
    // Set expected values for registers that participate in the loop exit
    // condition (we execute only the first iteration of the loop). See
    // addIncomingValues/getIncomingValues description for additional details.
    assert(SLI);
    for (const auto &[Reg, Value] : SLI->getIncomingValues(MBB))
      I.setReg(Reg, Value);
  }

  LLVM_DEBUG(
      dbgs() << "Instructions generation with enabled tracking on model: "
             << NotVisited.size() << " basic block are dead.\n");

  assert((NotVisited.empty() || SimCtx.hasTrackingMode()) &&
         "At this point some basic block might not be visited only when model "
         "controls generation routine.");

  if (SimCtx.hasTrackingMode()) {
    // Model must not be used further as it finished execution.
    SimCtx.disableTrackingMode();
    auto &I = SimCtx.getInterpreter();
    I.disableTransactionsTracking();
    I.resetMem();
  }

  MBB = findNextBlock(nullptr, NotVisited, nullptr, State, SimCtx);
  while (!FunctionGenRequest.isLimitReached(CurrMFGenStats)) {
    assert(MBB);
    assert(!NotVisited.empty() &&
           "There are no more block to insert instructions.");
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.at(MBB);
    SNIPPY_DEBUG_BRIEF("request for dead BasicBlock", BBReq);

    planning::InstructionGenerationContext InstrGenCtx{*MBB, MBB->begin(), GC,
                                                       SimCtx};
    InstrGenCtx.append(&CGS).append(MAI).append(SFM);
    auto RP = InstrGenCtx.pushRegPool();
    snippy::generate(BBReq, InstrGenCtx);

    CurrMFGenStats.merge(InstrGenCtx.Stats);
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);
    MBB = findNextBlock(nullptr, NotVisited, nullptr, State, SimCtx);
  }

  finalizeFunction(MF, FunctionGenRequest, CurrMFGenStats, GC, SimCtx, CGS,
                   MAI);
}

void generate(planning::InstructionGroupRequest &IG,
              planning::InstructionGenerationContext &InstrGenCtx) {
  LLVM_DEBUG(dbgs() << "Generating IG:\n"; IG.print(dbgs(), 2););
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &MBB = InstrGenCtx.MBB;
  auto OldStats = InstrGenCtx.Stats;
  InstrGenCtx.Stats = GenerationStatistics();
  if (GenerateInsertionPointHints)
    generateInsertionPointHint(InstrGenCtx);
  auto ItEnd = InstrGenCtx.Ins;
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  {
    auto RP = InstrGenCtx.pushRegPool();
    auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    planning::InstrGroupGenerationRAIIWrapper InitAndFinish(IG, InstrGenCtx);
    auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
    planning::InstrRequestRange Range(IG, InstrGenCtx.Stats);
    for (auto &&IR : Range) {
      if (GenerateInsertionPointHints && !IG.isInseparableBundle())
        generateInsertionPointHint(InstrGenCtx);
      generateInstruction(InstrInfo.get(IR.Opcode), InstrGenCtx,
                          std::move(IR.Preselected));
      switchPolicyIfNeeded(InstrGenCtx, IG, IR.Opcode);
      if (!IG.isInseparableBundle())
        ItBegin =
            processGeneratedInstructions(ItBegin, InstrGenCtx, IG.limit());
    }
  }
  // If instructions were not already postprocessed.
  if (IG.isInseparableBundle())
    processGeneratedInstructions(ItBegin, InstrGenCtx, IG.limit());
  if (!IG.isLimitReached(InstrGenCtx.Stats)) {
    auto &Ctx = ProgCtx.getLLVMState().getCtx();
    snippy::fatal(Ctx, "Generation failure",
                  "No more instructions to request but group limit still "
                  "hasn't been reached.");
  }
  InstrGenCtx.Stats.merge(OldStats);
}

namespace {

void interpretMBBInstrs(LLVMState &State, const SimulatorContext &SimCtx,
                        MachineBasicBlock::const_iterator BeginInterpretIter,
                        MachineBasicBlock::const_iterator EndInterpretIter) {
  auto Res = snippy::interpretInstrs(BeginInterpretIter, EndInterpretIter,
                                     State, SimCtx);
  if (Res != GenerationStatus::Ok)
    snippy::fatal(State.getCtx(), "instruction interpretation error",
                  "Interpretation failed on instructions inserted before "
                  "main instructions generation routine.");
}

} // namespace

GenerationStatistics
generate(planning::BasicBlockRequest &BB,
         planning::InstructionGenerationContext &InstrGenCtx) {
  for (auto &&IG : BB)
    generate(IG, InstrGenCtx);

  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &SimCtx = InstrGenCtx.SimCtx;
  const auto &MBB = BB.getMBB();
  // In the register block we start interpretation from the beginning
  // In other blocks we begin from a pre-established iterator (Ins)
  if (SimCtx.hasTrackingMode()) {
    auto *SFM = InstrGenCtx.SFM;
    if (!SFM || (SFM->RegsInitBlock == &MBB))
      interpretMBBInstrs(State, SimCtx, MBB.begin(), MBB.getFirstTerminator());
  }
  return InstrGenCtx.Stats;
}
} // namespace snippy
} // namespace llvm
