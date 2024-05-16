//===-- FlowGenerator.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FlowGenerator.h"
#include "BlockGenPlanningPass.h"
#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/Backtrack.h"
#include "snippy/Generator/CallGraphState.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/IntervalsToVerify.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/PassManagerWrapper.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

#include <functional>
#include <sstream>
#include <tuple>

#define DEBUG_TYPE "snippy-flow-generator"
#define PASS_DESC "Snippy Flow Generator"

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

extern cl::OptionCategory Options;

static snippy::opt<std::string>
    RegGeneratorFile("reg-generator-plugin",
                     cl::desc("Plugin for custom registers generation."
                              "Use =None to generate registers "
                              "with build-in randomizer."
                              "(=None - default value)"),
                     cl::value_desc("filename"), cl::cat(Options),
                     cl::init("None"));

static snippy::opt<std::string>
    RegInfoFile("reg-plugin-info-file",
                cl::desc("File with info for registers generator. "
                         "Use =None if plugin doesn't need additional info."
                         "(=None - default value)"),
                cl::value_desc("filename"), cl::cat(Options), cl::init("None"));

static snippy::opt<unsigned>
    BTThreshold("bt-threshold",
                cl::desc("the maximal number of back-tracking events"),
                cl::Hidden, cl::init(10000));

static snippy::opt<unsigned> SizeErrorThreshold(
    "size-error-threshold",
    cl::desc("the maximal number of attempts to generate code fitting size"),
    cl::Hidden, cl::init(5));

static snippy::opt<std::string>
    DumpMIR("dump-mir", cl::ValueOptional,
            cl::desc("Request dump the Machine IR."),
            cl::value_desc("filename"), cl::Hidden, cl::init("a.mir"),
            cl::cat(Options));

struct CallGraphDumpEnumOption
    : public snippy::EnumOptionMixin<CallGraphDumpEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(CallGraphDumpMode::Dot, "dot",
                    "is used to render visual graph representation");
    Mapper.enumCase(CallGraphDumpMode::Yaml, "yaml",
                    "can be read back by snippy");
  }
};

static snippy::opt<CallGraphDumpMode>
    CGDumpFormat("call-graph-dump-format",
                 cl::desc("Choose format for call graph dump option:"),
                 CallGraphDumpEnumOption::getClValues(),
                 cl::init(CallGraphDumpMode::Dot), cl::cat(Options));

snippy::opt<bool> VerifyConsecutiveLoops(
    "verify-consecutive-loops",
    cl::desc(
        "Check that consecutive loops generated accordingly to branchegram."),
    cl::cat(Options), cl::init(false), cl::Hidden);

} // namespace snippy

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::CallGraphDumpMode,
                                           snippy::CallGraphDumpEnumOption)

namespace snippy {

static snippy::opt<std::string>
    DumpCGFilename("call-graph-dump-filename",
                   cl::desc("Specify file to dump call graph in dot format"),
                   cl::value_desc("filename"), cl::init(""), cl::cat(Options));
static snippy::opt<bool> SelfCheckGV(
    "selfcheck-gv",
    cl::desc("add selfcheck section properties such as VMA, size and stride as "
             "a global constants with an external linkage"),
    cl::Hidden, cl::init(false));
static snippy::opt<bool> ExportGV(
    "export-gv",
    cl::desc(
        "add sections properties such as VMA, size and stride as "
        "a global constants with an external linkage. Requires a model plugin"),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    GenerateInsertionPointHints("generate-insertion-point-hints",
                                cl::desc("Generate hint instructions in places "
                                         "where it's safe to mutate snippet"),
                                cl::Hidden, cl::init(false));

static snippy::opt<std::string>
    RegionsToVerify("dump-intervals-to-verify",
                    cl::desc("Save PC intervals that can be verified (YAML)"),
                    cl::cat(Options), cl::ValueOptional, cl::init(""));

enum class GenerationStatus {
  Ok,
  BacktrackingFailed,
  InterpretFailed,
  SizeFailed,
};

struct GenerationResult {
  GenerationStatus Status;
  GenerationStatistics Stats;
};

namespace {

void writeMIRFile(StringRef Data) {
  auto Path = DumpMIR.getValue();
  if (Path.empty())
    Path = DumpMIR.getDefault().getValue();
  writeFile(Path, Data);
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

template <typename RetType, RetType AsOne, RetType AsZero>
class AsOneGenerator {
  unsigned long long Period;
  mutable unsigned long long State;

public:
  AsOneGenerator(unsigned long long Period = 0)
      : Period(Period), State(Period) {}

  auto operator()() const {
    if (!State)
      return AsZero;

    --State;
    if (State)
      return AsZero;

    State = Period;
    return AsOne;
  }
};

class InstructionGenerator final : public MachineFunctionPass {
  GenerationResult generateNopsToSizeLimit(
      MachineBasicBlock &MBB,
      const planning::InstructionGroupRequest &InstructionGroupRequest,
      GenerationStatistics &CurrentInstructionGroupStats);

  void generateInstruction(
      const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
      MachineBasicBlock::iterator Ins,
      std::vector<planning::PreselectedOpInfo> Preselected = {});

  template <typename InstrIt>
  GenerationStatus interpretInstrs(InstrIt Begin, InstrIt End) const;

  template <typename InstrIt>
  std::vector<InstrIt> collectSelfcheckCandidates(InstrIt Begin,
                                                  InstrIt End) const;

  template <typename InstrIt>
  GenerationResult handleGeneratedInstructions(
      MachineBasicBlock &MBB, InstrIt ItBegin, InstrIt ItEnd,
      const planning::InstructionGroupRequest &InstructionGroupRequest,
      const GenerationStatistics &CurrentInstructionGroupStats) const;

  template <typename InstrIt>
  MachineBasicBlock::iterator processGeneratedInstructions(
      MachineBasicBlock &MBB, InstrIt ItBegin,
      const planning::InstructionGroupRequest &IG, unsigned &BacktrackCount,
      unsigned &SizeErrorCount, GenerationStatistics &CurrentStats);

  void processGenerationResult(MachineBasicBlock &MBB,
                               const planning::InstructionGroupRequest &IG,
                               const GenerationResult &IntRes,
                               GenerationStatistics &CurrentStats,
                               unsigned &BacktrackCount,
                               unsigned &SizeErrorCount);

  void selfcheckOverflowGuard() const;

  template <typename InstrIt>
  void storeRefValue(MachineBasicBlock &MBB, InstrIt InsertPos, MemAddr Addr,
                     APInt Val, RegPoolWrapper &RP) const;

  // Returns iterator to the first inserted instruction. So [returned It;
  // InsertPos) contains inserted instructions.
  // Register pool is taken by value as we'll do some local reservations on top
  // of the existing register pool. All changes made must be canceled at the
  // function exit.
  template <typename InstrIt>
  SelfcheckIntermediateInfo<InstrIt>
  storeRefAndActualValueForSelfcheck(MachineBasicBlock &MBB, InstrIt InsertPos,
                                     Register DestReg, RegPoolWrapper RP) const;

  MachineInstr *randomInstruction(
      const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
      MachineBasicBlock::iterator Ins,
      std::vector<planning::PreselectedOpInfo> Preselected = {}) const;

  void generate(planning::InstructionGroupRequest &IG, MachineBasicBlock &MBB,
                GenerationStatistics &CurrentStats);

  GenerationStatistics generate(planning::BasicBlockRequest &Request,
                                MachineBasicBlock &MBB);

  MachineBasicBlock *
  findNextBlock(MachineBasicBlock *MBB,
                const std::set<MachineBasicBlock *, MIRComp> &NotVisited,
                const MachineLoop *ML = nullptr) const;

  GenerationStatistics generateCompensationCode(MachineBasicBlock &MBB);

  MachineBasicBlock *findNextBlockOnModel(MachineBasicBlock &MBB) const;

  planning::FunctionRequest
  createMFGenerationRequest(const MachineFunction &MF) const;

  void generate(planning::FunctionRequest &Request, MachineFunction &MF);

  void finalizeFunction(MachineFunction &MF, planning::FunctionRequest &Request,
                        const GenerationStatistics &MFStats);

  unsigned chooseAddressRegister(MachineInstr &MI, const AddressPart &AP,
                                 RegPoolWrapper &RP) const;

  void postprocessMemoryOperands(MachineInstr &MI, RegPoolWrapper &RP) const;

  bool preInterpretBacktracking(const MachineInstr &MI) const;

  void prepareInterpreterEnv() const;

  void addGV(const APInt &Value, unsigned long long Stride,
             GlobalValue::LinkageTypes LType, StringRef Name) const;

  void addSelfcheckSectionPropertiesAsGV() const;

  void addModelMemoryPropertiesAsGV() const;

  // Attempt to generate valid operands
  std::optional<SmallVector<MachineOperand, 8>> tryToPregenerateOperands(
      const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
      RegPoolWrapper &RP,
      const std::vector<planning::PreselectedOpInfo> &Preselected) const;

  // In order to support register plugin InstructionGenerator pre-generates all
  //  instructio operands.
  // Random generator always generates valid registers,
  //  but plugin may abort this process on an intermediate operand.
  // In this case InstructionGenerator starts generation from the first operand
  //  of the current instruction.
  SmallVector<MachineOperand, 8> pregenerateOperands(
      const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
      RegPoolWrapper &RP,
      const std::vector<planning::PreselectedOpInfo> &Preselected) const;

  // Generates operand and stores it into the queue.
  std::optional<MachineOperand> pregenerateOneOperand(
      const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
      RegPoolWrapper &RP, const MCOperandInfo &MCOpInfo,
      const planning::PreselectedOpInfo &Preselected, unsigned OperandIndex,
      ArrayRef<MachineOperand> PregeneratedOperands) const;

  void generateCall(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                    unsigned OpCode) const;

  void generateInsertionPointHint(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator Ins) const;

  GeneratorContext *SGCtx;
  AsOneGenerator<bool, true, false> SelfcheckPeriodTracker;
  std::unique_ptr<Backtrack> BT;

  mutable unsigned long long SCAddress = 0;
  std::unique_ptr<Backtrack> createBacktrack() const;

public:
  static char ID;

  InstructionGenerator();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<BlockGenPlanning>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

char InstructionGenerator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::InstructionGenerator;

INITIALIZE_PASS_BEGIN(InstructionGenerator, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(BlockGenPlanning)
INITIALIZE_PASS_END(InstructionGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createInstructionGeneratorPass() {
  return new InstructionGenerator();
}

namespace snippy {

InstructionGenerator::InstructionGenerator() : MachineFunctionPass(ID) {
  initializeInstructionGeneratorPass(*PassRegistry::getPassRegistry());
}

void InstructionGenerator::generateInstruction(
    const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator Ins,
    std::vector<planning::PreselectedOpInfo> Preselected) {
  auto &State = SGCtx->getLLVMState();
  auto Opc = InstrDesc.getOpcode();
  const auto &SnippyTgt = State.getSnippyTarget();

  assert(!InstrDesc.isBranch() &&
         "All branch instructions expected to be generated separately");

  if (SnippyTgt.requiresCustomGeneration(InstrDesc)) {
    SnippyTgt.generateCustomInst(InstrDesc, MBB, *SGCtx, Ins);
    return;
  }

  if (SnippyTgt.isCall(Opc))
    generateCall(MBB, Ins, Opc);
  else
    randomInstruction(InstrDesc, MBB, Ins, std::move(Preselected));
}

std::unique_ptr<Backtrack> InstructionGenerator::createBacktrack() const {
  return std::make_unique<Backtrack>(SGCtx->getOrCreateInterpreter());
}

bool InstructionGenerator::preInterpretBacktracking(
    const MachineInstr &MI) const {
  assert(BT);
  if (!BT->isInstrValid(MI, SGCtx->getLLVMState().getSnippyTarget()))
    return false;
  // TODO: maybe there will be another one way for back-tracking?
  return true;
}

// This must always be in sync with prologue epilogue insertion.
static size_t calcMainFuncInitialSpillSize(GeneratorContext &GC) {
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto StackPointer = SnippyTgt.getStackPointer();
  size_t SpillSize = SnippyTgt.getSpillAlignmentInBytes(StackPointer, GC);

  auto SpilledRegs = GC.getSpilledRegs();
  return std::accumulate(SpilledRegs.begin(), SpilledRegs.end(), SpillSize,
                         [&GC, &SnippyTgt](auto Init, auto Reg) {
                           return Init + SnippyTgt.getSpillSizeInBytes(Reg, GC);
                         });
}

void InstructionGenerator::prepareInterpreterEnv() const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &I = SGCtx->getOrCreateInterpreter();

  I.setInitialState(SGCtx->getInitialRegisterState(I.getSubTarget()));
  if (!SGCtx->hasStackSection())
    return;
  // Prologue insertion happens after instructions generation, so we do not
  // have SP initialization instructions at this point. However, we know the
  // actual value of SP, so let's initialize it in model artificially.
  auto SP = SnippyTgt.getStackPointer();
  APInt StackTop(SnippyTgt.getRegBitWidth(SP, *SGCtx),
                 SGCtx->getStackTop() - calcMainFuncInitialSpillSize(*SGCtx));
  I.setReg(SP, StackTop);
}

void InstructionGenerator::addGV(
    const APInt &Value, unsigned long long Stride = 1,
    GlobalValue::LinkageTypes LType = GlobalValue::ExternalLinkage,
    StringRef Name = "global") const {
  auto &GP = SGCtx->getGlobalsPool();

  const auto *GV = GP.createGV(Value, Stride, LType, Name);
  if (SGCtx->hasTrackingMode())
    SGCtx->getOrCreateInterpreter().writeMem(GP.getGVAddress(GV), Value);
}

void InstructionGenerator::addModelMemoryPropertiesAsGV() const {
  auto MemCfg = MemoryConfig::getMemoryConfig(SGCtx->getLinker());
  // Below we add all the model memory properties as global constants
  constexpr auto ConstantSizeInBits = 64u; // Constants size in bits
  constexpr auto Alignment = 1u;           // Without special alignment

  auto DataSectionVMA = APInt{ConstantSizeInBits, MemCfg.Ram.Start};
  addGV(DataSectionVMA, Alignment, GlobalValue::ExternalLinkage,
        "data_section_address");

  auto DataSectionSize = APInt{ConstantSizeInBits, MemCfg.Ram.Size};
  addGV(DataSectionSize, Alignment, GlobalValue::ExternalLinkage,
        "data_section_size");

  auto ExecSectionVMA =
      APInt{ConstantSizeInBits, MemCfg.ProgSections.front().Start};
  addGV(ExecSectionVMA, Alignment, GlobalValue::ExternalLinkage,
        "exec_section_address");

  auto ExecSectionSize =
      APInt{ConstantSizeInBits, MemCfg.ProgSections.front().Size};
  addGV(ExecSectionSize, Alignment, GlobalValue::ExternalLinkage,
        "exec_section_size");
}

void InstructionGenerator::addSelfcheckSectionPropertiesAsGV() const {
  const auto &SelfcheckSection = SGCtx->getSelfcheckSection();

  auto VMA = APInt{64, SelfcheckSection.VMA};
  auto Size = APInt{64, SelfcheckSection.Size};
  auto Stride = APInt{64, SGCtx->getSCStride()};

  addGV(VMA, 1, GlobalValue::ExternalLinkage, "selfcheck_section_address");
  addGV(Size, 1, GlobalValue::ExternalLinkage, "selfcheck_section_size");
  addGV(Stride, 1, GlobalValue::ExternalLinkage, "selfcheck_data_byte_stride");
}

template <typename InstrIt>
GenerationStatus InstructionGenerator::interpretInstrs(InstrIt Begin,
                                                       InstrIt End) const {
  auto &I = SGCtx->getOrCreateInterpreter();
  const auto &State = SGCtx->getLLVMState();
  assert(SGCtx->hasTrackingMode());

  for (auto &MI : iterator_range(Begin, End)) {
    if (BT && !preInterpretBacktracking(MI))
      return GenerationStatus::BacktrackingFailed;
    I.addInstr(MI, State);
    if (I.step() != ExecutionResult::Success)
      return GenerationStatus::InterpretFailed;
  }
  return GenerationStatus::Ok;
}

// TODO: We need other ways of this function implemetation
template <typename InstrIt>
std::vector<InstrIt>
InstructionGenerator::collectSelfcheckCandidates(InstrIt Begin,
                                                 InstrIt End) const {
  const auto &ST = SGCtx->getLLVMState().getSnippyTarget();
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

static bool sizeLimitIsReached(const planning::RequestLimit &Lim,
                               const GenerationStatistics &CommitedStats,
                               size_t NewGeneratedCodeSize) {
  if (!Lim.isSizeLimit())
    return false;
  return NewGeneratedCodeSize > Lim.getSizeLeft(CommitedStats);
}

template <typename InstrIt>
GenerationResult InstructionGenerator::handleGeneratedInstructions(
    MachineBasicBlock &MBB, InstrIt ItBegin, InstrIt ItEnd,
    const planning::InstructionGroupRequest &InstructionGroupRequest,
    const GenerationStatistics &CurrentInstructionGroupStats) const {
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
  if (sizeLimitIsReached(InstructionGroupRequest.limit(),
                         CurrentInstructionGroupStats, GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  if (!SGCtx->hasTrackingMode())
    return ReportGenerationResult(GenerationStatus::Ok);

  auto InterpretInstrsResult = interpretInstrs(ItBegin, ItEnd);
  if (InterpretInstrsResult != GenerationStatus::Ok)
    return ReportGenerationResult(InterpretInstrsResult);

  const auto &GenSettings = SGCtx->getGenSettings();
  if (!GenSettings.TrackingConfig.SelfCheckPeriod)
    return ReportGenerationResult(GenerationStatus::Ok);

  if (PrimaryInstrCount == 0) {
    LLVM_DEBUG(
        dbgs() << "Ignoring selfcheck for the supportive instruction\n";);
    return ReportGenerationResult(GenerationStatus::Ok);
  }

  assert(PrimaryInstrCount >= 1);

  // Collect instructions that can and should be selfchecked.
  auto SelfcheckCandidates = collectSelfcheckCandidates(ItBegin, ItEnd);

  // Collect def registers from the candidates.
  // Use SetVector as we want to preserve order of insertion.
  SetVector<SelfcheckAnnotationInfo<InstrIt>> Defs;
  const auto &ST = SGCtx->getLLVMState().getSnippyTarget();
  // Reverse traversal as we need to sort definitions by their last use.
  for (auto Inst : reverse(SelfcheckCandidates)) {
    // TODO: Add self-check for instructions without definitions
    if (Inst->getDesc().getNumDefs() == 0)
      continue;
    auto Regs = ST.getRegsForSelfcheck(*Inst, MBB, *SGCtx);
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
  auto RP = SGCtx->getRegisterPool();
  std::vector<SelfcheckFirstStoreInfo<InstrIt>> StoresInfo;
  for (const auto &Def : Defs) {
    RP.addReserved(Def.DestReg);
    auto SelfcheckInterInfo = storeRefAndActualValueForSelfcheck(
        MBB, InsPoint, Def.DestReg, RP.push());
    InsPoint = SelfcheckInterInfo.NextInsertPos;
    // Collect information about first generated selfcehck store for a primary
    // instr
    StoresInfo.push_back(SelfcheckInterInfo.FirstStoreInfo);
  }

  assert(Defs.size() == StoresInfo.size());
  // Add collected information about generated selfcheck stores for selfcheck
  // annotation
  for (const auto &[Def, StoreInfo] : zip(Defs, StoresInfo))
    SGCtx->addToSelfcheckMap(
        StoreInfo.Address,
        std::distance(Def.Inst, std::next(StoreInfo.FirstStoreInstrPos)));
  // Execute self-check instructions
  auto &I = SGCtx->getOrCreateInterpreter();
  if (!I.executeChainOfInstrs(SGCtx->getLLVMState(), InsPoint, ItEnd))
    I.reportSimulationFatalError(
        "Failed to execute chain of instructions in tracking mode");

  // Check size requirements after selfcheck addition.
  GeneratedCodeSize = getCodeSize(ItBegin, ItEnd);
  if (sizeLimitIsReached(InstructionGroupRequest.limit(),
                         CurrentInstructionGroupStats, GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  return ReportGenerationResult(GenerationStatus::Ok);
}

GenerationResult InstructionGenerator::generateNopsToSizeLimit(
    MachineBasicBlock &MBB,
    const planning::InstructionGroupRequest &InstructionGroupRequest,
    GenerationStatistics &CurrentInstructionGroupStats) {
  assert(InstructionGroupRequest.limit().isSizeLimit());
  auto &State = SGCtx->getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto ItEnd = MBB.getFirstTerminator();
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  while (
      !InstructionGroupRequest.isLimitReached(CurrentInstructionGroupStats)) {
    auto *MI = SnpTgt.generateNop(MBB, ItBegin, State);
    assert(MI && "Nop generation failed");
    CurrentInstructionGroupStats.merge(
        GenerationStatistics(0, MI->getDesc().getSize()));
  }
  ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
  return handleGeneratedInstructions(MBB, ItBegin, ItEnd,
                                     InstructionGroupRequest,
                                     CurrentInstructionGroupStats);
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

template <typename T>
static void printDebugBrief(raw_ostream &OS, const Twine &Brief,
                            const T &Entity, int Indent = 0) {
  OS.indent(Indent) << Brief << ": ";
  Entity.print(OS);
  OS << "\n";
}

#ifdef SNIPPY_DEBUG_BRIEF
#error macro SNIPPY_DEBUG_BRIEF already defined!
#endif

#define SNIPPY_DEBUG_BRIEF(...) LLVM_DEBUG(printDebugBrief(dbgs(), __VA_ARGS__))

void InstructionGenerator::selfcheckOverflowGuard() const {
  const auto &SelfcheckSection = SGCtx->getSelfcheckSection();
  if (SCAddress >= SelfcheckSection.VMA + SelfcheckSection.Size)
    report_fatal_error("Selfcheck section overflow. Try to provide "
                       "\"selfcheck\" section description in layout file",
                       false);
}

template <typename InstrIt>
void InstructionGenerator::storeRefValue(MachineBasicBlock &MBB,
                                         InstrIt InsertPos, MemAddr Addr,
                                         APInt Val, RegPoolWrapper &RP) const {
  SGCtx->getLLVMState().getSnippyTarget().storeValueToAddr(
      MBB, InsertPos, SCAddress, Val, RP, *SGCtx);
}

template <typename InstrIt>
SelfcheckIntermediateInfo<InstrIt>
InstructionGenerator::storeRefAndActualValueForSelfcheck(
    MachineBasicBlock &MBB, InstrIt InsertPos, Register DestReg,
    RegPoolWrapper RP) const {
  const auto &ST = SGCtx->getLLVMState().getSnippyTarget();
  auto &I = SGCtx->getOrCreateInterpreter();
  auto RegValue = I.readReg(DestReg);

  bool IsBegin = MBB.begin() == InsertPos;
  auto FirstInserted = InsertPos;
  if (!IsBegin)
    FirstInserted = std::prev(InsertPos);

  ST.storeRegToAddr(MBB, InsertPos, SCAddress, DestReg, RP, *SGCtx,
                    /* store the whole register */ 0);
  SelfcheckFirstStoreInfo<InstrIt> FirstStoreInfo{std::prev(InsertPos),
                                                  SCAddress};
  SCAddress += SGCtx->getSCStride();
  selfcheckOverflowGuard();
  storeRefValue(MBB, InsertPos, SCAddress, RegValue, RP);
  SCAddress += SGCtx->getSCStride();
  selfcheckOverflowGuard();

  if (IsBegin)
    return {MBB.begin(), FirstStoreInfo};
  return {std::next(FirstInserted), FirstStoreInfo};
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

std::optional<MachineOperand> InstructionGenerator::pregenerateOneOperand(
    const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP, const MCOperandInfo &MCOpInfo,
    const planning::PreselectedOpInfo &Preselected, unsigned OpIndex,
    ArrayRef<MachineOperand> PregeneratedOperands) const {
  auto OpType = MCOpInfo.OperandType;

  const auto &State = SGCtx->getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RegGen = SGCtx->getRegGen();

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
          SnippyTgt.getRegClass(*SGCtx, OperandRegClassID, OpIndex,
                                InstrDesc.getOpcode(), MBB, RegInfo);
      auto Exclude =
          SnippyTgt.excludeRegsForOperand(RegClass, *SGCtx, InstrDesc, OpIndex);
      auto Include = SnippyTgt.includeRegs(RegClass);
      AccessMaskBit Mask = IsDst ? AccessMaskBit::W : AccessMaskBit::R;

      auto CustomMask =
          SnippyTgt.getCustomAccessMaskForOperand(*SGCtx, InstrDesc, OpIndex);
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
                                  /* isMem */ false, Reg, RP, *SGCtx, MBB);
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
      auto RegClass =
          SnippyTgt.getRegClass(*SGCtx, OperandRegClassID, OpIndex,
                                InstrDesc.getOpcode(), MBB, RegInfo);
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
                                  /* isMem */ true, Reg, RP, *SGCtx, MBB);
    return createRegAsOperand(Reg, Preselected.getFlags());
  }

  if (OpType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
    assert(Preselected.isUnset() || Preselected.isImm());
    StridedImmediate StridedImm;
    if (Preselected.isImm())
      StridedImm = Preselected.getImm();
    return SnippyTgt.generateTargetOperand(*SGCtx, InstrDesc.getOpcode(),
                                           OpType, StridedImm);
  }
  llvm_unreachable("this operand type unsupported");
}

static SmallVector<unsigned, 4> pickRecentDefs(MachineInstr &MI,
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

unsigned InstructionGenerator::chooseAddressRegister(MachineInstr &MI,
                                                     const AddressPart &AP,
                                                     RegPoolWrapper &RP) const {
  auto &State = SGCtx->getLLVMState();
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

  auto &I = SGCtx->getOrCreateInterpreter();
  auto AddrValue = AP.Value;

  MinimalMatCost = std::accumulate(
      PickedRegisters.begin(), PickedRegisters.end(), MinimalMatCost,
      [&I, &SnippyTgt, &AddrValue, SGCtx = SGCtx](auto CurCost, auto &&Reg) {
        auto NewCost = CurCost;
        auto MatCost = SnippyTgt.getTransformSequenceLength(
            I.readReg(Reg), AddrValue, Reg, *SGCtx);
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

static std::pair<AddressInfo, AddressGenInfo>
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

  auto Result =
      MS.randomAddressForInstructions(AccessSize, Alignment, ChooseAddrGenInfo);
  const auto &AddrInfo = std::get<AddressInfo>(Result);

  assert(AddrInfo.MaxOffset >= 0);
  assert(AddrInfo.MinOffset <= 0);

  return Result;
}

void InstructionGenerator::postprocessMemoryOperands(MachineInstr &MI,
                                                     RegPoolWrapper &RP) const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto Opcode = MI.getDesc().getOpcode();
  auto NumAddrsToGen = SnippyTgt.countAddrsToGenerate(Opcode);
  auto &MLI = getAnalysis<MachineLoopInfo>();
  auto *MBB = MI.getParent();
  auto *ML = MLI.getLoopFor(MBB);

  for (size_t i = 0; i < NumAddrsToGen; ++i) {
    auto [AddrInfo, AddrGenInfo] = chooseAddrInfoForInstr(MI, *SGCtx, ML);
    bool GenerateStridedLoad = !AddrGenInfo.isSingleElement();
    auto AccessSize = AddrInfo.AccessSize;

    if (GenerateStridedLoad && SGCtx->hasTrackingMode())
      snippy::fatal(State.getCtx(), "Incompatible features",
                    "Can't use generate strided accesses in loops with "
                    "tracking mode enabled");

    auto &&[RegToValue, ChosenAddresses] =
        SnippyTgt.breakDownAddr(AddrInfo, MI, i, *SGCtx);

    for (unsigned j = 0; j < AddrGenInfo.NumElements; ++j) {
      addMemAccessToDump(ChosenAddresses, *SGCtx, AccessSize);

      for_each(ChosenAddresses,
               [Stride = AddrInfo.MinStride](auto &Val) { Val += Stride; });
    }

    auto InsPoint = MI.getIterator();
    for (auto &AP : RegToValue) {
      MCRegister Reg = AP.FixedReg;
      if (SGCtx->hasTrackingMode() &&
          SGCtx->getGenSettings().TrackingConfig.AddressVH) {
        auto &I = SGCtx->getOrCreateInterpreter();
        Reg = chooseAddressRegister(MI, AP, RP);
        SnippyTgt.transformValueInReg(*MI.getParent(), InsPoint, I.readReg(Reg),
                                      AP.Value, Reg, RP, *SGCtx);
      } else {
        SnippyTgt.writeValueToReg(*MI.getParent(), InsPoint, AP.Value, Reg, RP,
                                  *SGCtx);
      }
      // Address registers must not be overwritten during other memory operands
      // preparation.
      RP.addReserved(Reg, AccessMaskBit::W);
    }
  }
}

void InstructionGenerator::generateCall(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator Ins,
                                        unsigned OpCode) const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &CGS = SGCtx->getCallGraphState();
  auto *Node = CGS.getNode(&(MBB.getParent()->getFunction()));
  auto CalleeCount = Node->callees().size();
  if (!CalleeCount)
    return;
  auto CalleeIdx = RandEngine::genInRange(CalleeCount);
  auto *CalleeNode = std::next(Node->callees().begin(), CalleeIdx)->Dest;
  auto FunctionIdx = RandEngine::genInRange(CalleeNode->functions().size());

  auto &CallTarget = *(CalleeNode->functions()[FunctionIdx]);

  SnippyTgt.generateCall(MBB, Ins, CallTarget, *SGCtx, /* AsSupport */ false,
                         OpCode);
  Node->markAsCommitted(CalleeNode);
}

void InstructionGenerator::generateInsertionPointHint(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins) const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  SnippyTgt.generateNop(MBB, Ins, State);
}

std::optional<SmallVector<MachineOperand, 8>>
InstructionGenerator::tryToPregenerateOperands(
    const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP,
    const std::vector<planning::PreselectedOpInfo> &Preselected) const {
  SmallVector<MachineOperand, 8> PregeneratedOperands;
  assert(InstrDesc.getNumOperands() == Preselected.size());
  iota_range<unsigned long> PreIota(0, Preselected.size(),
                                    /* Inclusive */ false);
  for (const auto &[MCOpInfo, PreselOpInfo, Index] :
       zip(InstrDesc.operands(), Preselected, PreIota)) {
    auto OpOpt =
        pregenerateOneOperand(MBB, InstrDesc, RP, MCOpInfo, PreselOpInfo, Index,
                              PregeneratedOperands);
    if (!OpOpt)
      return std::nullopt;
    PregeneratedOperands.push_back(OpOpt.value());
  }
  return PregeneratedOperands;
}

SmallVector<MachineOperand, 8> InstructionGenerator::pregenerateOperands(
    const MachineBasicBlock &MBB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP,
    const std::vector<planning::PreselectedOpInfo> &Preselected) const {
  auto &RegGenerator = SGCtx->getRegGen();
  auto &State = SGCtx->getLLVMState();
  auto &InstrInfo = State.getInstrInfo();

  auto IterNum = 0u;
  constexpr auto FailsMaxNum = 10000u;

  while (IterNum < FailsMaxNum) {
    RP.reset();
    RegGenerator.setRegContextForPlugin();
    auto PregeneratedOperandsOpt =
        tryToPregenerateOperands(MBB, InstrDesc, RP, Preselected);
    IterNum++;

    if (PregeneratedOperandsOpt)
      return std::move(PregeneratedOperandsOpt.value());
  }

  report_fatal_error("Limit on failed generation attempts exceeded "
                     "on instruction " +
                         InstrInfo.getName(InstrDesc.getOpcode()),
                     false);
}

MachineInstr *InstructionGenerator::randomInstruction(
    const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator Ins,
    std::vector<planning::PreselectedOpInfo> Preselected) const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto MIB = BuildMI(MBB, Ins, MIMetadata(), InstrDesc);

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

  auto RP = SGCtx->getRegisterPool();
  auto PregeneratedOperands =
      pregenerateOperands(MBB, InstrDesc, RP, Preselected);
  for (auto &Op : PregeneratedOperands)
    MIB.add(Op);

  if (DoPostprocess)
    postprocessMemoryOperands(*MIB, RP);
  // FIXME:
  // We have a lot of problems with rollback and configurations
  // After this, we can have additional instruction after main one!
  auto PostProcessInstPos = std::next(MIB->getIterator());
  assert(PostProcessInstPos == Ins);
  SnippyTgt.instructionPostProcess(*MIB, *SGCtx, PostProcessInstPos);
  return MIB;
}

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

void InstructionGenerator::processGenerationResult(
    MachineBasicBlock &MBB, const planning::InstructionGroupRequest &IG,
    const GenerationResult &IntRes, GenerationStatistics &CurrentStats,
    unsigned &BacktrackCount, unsigned &SizeErrorCount) {
  LLVM_DEBUG(printInterpretResult(dbgs(), "      ", IntRes); dbgs() << "\n");
  switch (IntRes.Status) {
  case GenerationStatus::InterpretFailed: {
    SGCtx->getOrCreateInterpreter().reportSimulationFatalError(
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
    auto GenRes = generateNopsToSizeLimit(MBB, IG, CurrentStats);
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

template <typename InstrIt>
MachineBasicBlock::iterator InstructionGenerator::processGeneratedInstructions(
    MachineBasicBlock &MBB, InstrIt ItBegin,
    const planning::InstructionGroupRequest &IG, unsigned &BacktrackCount,
    unsigned &SizeErrorCount, GenerationStatistics &CurrentStats) {
  auto ItEnd = MBB.getFirstTerminator();
  ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
  auto IntRes =
      handleGeneratedInstructions(MBB, ItBegin, ItEnd, IG, CurrentStats);
  processGenerationResult(MBB, IG, IntRes, CurrentStats, BacktrackCount,
                          SizeErrorCount);
  CurrentStats.merge(IntRes.Stats);
  return std::prev(ItEnd);
}

void InstructionGenerator::generate(planning::InstructionGroupRequest &IG,
                                    MachineBasicBlock &MBB,
                                    GenerationStatistics &CurrentStats) {
  LLVM_DEBUG(dbgs() << "Generating IG:\n"; IG.print(dbgs(), 2););
  auto BacktrackCount = 0u;
  auto SizeErrorCount = 0u;
  GenerationStatistics LocalStats{};
  if (GenerateInsertionPointHints)
    generateInsertionPointHint(MBB, MBB.getFirstTerminator());
  auto ItEnd = MBB.getFirstTerminator();
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  {
    auto RP = SGCtx->getRegisterPool();
    auto &InstrInfo = SGCtx->getLLVMState().getInstrInfo();
    planning::InstructionGenerationContext InstrGenCtx{MBB, ItEnd, RP, *SGCtx};
    planning::InstrGroupGenerationRAIIWrapper InitAndFinish(IG, InstrGenCtx);
    planning::InstrRequestRange Range(IG, LocalStats);
    for (auto &&IR : Range) {
      if (!IG.isInseparableBundle() && GenerateInsertionPointHints)
        generateInsertionPointHint(MBB, MBB.getFirstTerminator());
      generateInstruction(InstrInfo.get(IR.Opcode), MBB, ItEnd, IR.Preselected);
      switchPolicyIfNeeded(IG, IR.Opcode, MBB, *SGCtx);
      if (!IG.isInseparableBundle())
        ItBegin = processGeneratedInstructions(MBB, ItBegin, IG, BacktrackCount,
                                               SizeErrorCount, LocalStats);
    }
  }
  // Postprocess if instructions were not already postprocessed.
  if (IG.isInseparableBundle())
    processGeneratedInstructions(MBB, ItBegin, IG, BacktrackCount,
                                 SizeErrorCount, LocalStats);
  if (!IG.isLimitReached(LocalStats)) {
    auto &Ctx = SGCtx->getLLVMState().getCtx();
    snippy::fatal(Ctx, "Generation failure",
                  "No more instructions to request but group limit still "
                  "hasn't been reached.");
  }
  CurrentStats.merge(LocalStats);
}

GenerationStatistics
InstructionGenerator::generate(planning::BasicBlockRequest &BB,
                               MachineBasicBlock &MBB) {
  GenerationStatistics CurrentStats;
  for (auto &&IG : BB)
    generate(IG, MBB, CurrentStats);
  return CurrentStats;
}

MachineBasicBlock *InstructionGenerator::findNextBlock(
    MachineBasicBlock *MBB,
    const std::set<MachineBasicBlock *, MIRComp> &NotVisited,
    const MachineLoop *ML) const {
  if (SGCtx->hasTrackingMode() && MBB) {
    // In selfcheck/backtracking mode we go to exit block of the loop right
    // after the latch block.
    if (ML && ML->getLoopLatch() == MBB)
      return ML->getExitBlock();
    return findNextBlockOnModel(*MBB);
  }
  // When we're not tracking execution on the model or worrying about BBs order,
  // we can pick up any BB that we haven't processed yet.
  if (NotVisited.empty())
    return nullptr;
  return *NotVisited.begin();
}

MachineBasicBlock *
InstructionGenerator::findNextBlockOnModel(MachineBasicBlock &MBB) const {
  const auto &SnippyTgt = SGCtx->getLLVMState().getSnippyTarget();
  auto &I = SGCtx->getOrCreateInterpreter();
  auto PC = I.getPC();
  for (auto &Branch : MBB.terminators()) {
    assert(Branch.isBranch());
    if (Branch.isUnconditionalBranch())
      return SnippyTgt.getBranchDestination(Branch);

    assert(!Branch.isIndirectBranch() &&
           "Indirect branches are not supported for execution on the model "
           "during code generation.");
    assert(Branch.isConditionalBranch());
    I.addInstr(Branch, SGCtx->getLLVMState());

    if (I.step() != ExecutionResult::Success)
      I.reportSimulationFatalError(
          "Fail to execute generated instruction in tracking mode");

    if (I.getPC() == PC)
      // pc + 0. Conditional branch is taken (Asm printer replaces BB's label
      // with zero because the actual offset is still unknown).
      return SnippyTgt.getBranchDestination(Branch);
  }

  return MBB.getNextNode();

  llvm_unreachable(
      "Given BB doesn't have unconditional branch. All conditional branches "
      "were not taken. Successor is unknown.");
}

planning::FunctionRequest InstructionGenerator::createMFGenerationRequest(
    const MachineFunction &MF) const {
  auto &FunReq = getAnalysis<BlockGenPlanning>().get();
  const MCInstrDesc *FinalInstDesc = nullptr;
  auto LastInstrStr = SGCtx->getLastInstr();
  if (!LastInstrStr.empty() && !SGCtx->useRetAsLastInstr()) {
    auto Opc = SGCtx->getOpcodeCache().code(LastInstrStr.str());
    if (!Opc.has_value())
      report_fatal_error("unknown opcode \"" + Twine(LastInstrStr) +
                             "\" for last instruction generation",
                         false);
    FinalInstDesc = SGCtx->getOpcodeCache().desc(Opc.value());
  }
  FunReq.setFinalInstr(FinalInstDesc);
  return std::move(FunReq);
}

template <typename ValTy> APInt toAPInt(ValTy Val, unsigned Width) {
  return APInt(Width, Val);
}

template <> APInt toAPInt<APInt>(APInt Val, unsigned Width) {
  assert(Width >= Val.getBitWidth() && "Value is too long");
  return Val;
}

template <typename RegsSnapshotTy>
static void writeRegsSnapshot(RegsSnapshotTy RegsSnapshot,
                              MachineBasicBlock &MBB, RegStorageType Storage,
                              GeneratorContext &GC) {
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

GenerationStatistics
InstructionGenerator::generateCompensationCode(MachineBasicBlock &MBB) {
  assert(SGCtx->hasTrackingMode());

  const auto &SnippyTgt = SGCtx->getLLVMState().getSnippyTarget();

  auto &I = SGCtx->getOrCreateInterpreter();
  auto MemSnapshot = I.getMemBeforeTransaction();
  auto FRegsSnapshot = I.getFRegsBeforeTransaction();
  auto VRegsSnapshot = I.getVRegsBeforeTransaction();
  // Restoring memory requires GPR registers to prepare address and value to
  // write, so we need know which registers were changed. We cannot do it in the
  // current opened transaction as we'll mess up exiting from the loop state.
  // So, open a new one.
  I.openTransaction();
  if (!MemSnapshot.empty()) {
    auto StackSec = SGCtx->getStackSection();
    auto SelfcheckSec = SGCtx->getSelfcheckSection();
    for (auto [Addr, Data] : MemSnapshot) {
      assert(StackSec.Size > 0 && "Stack section cannot have zero size");
      // Skip stack and selfcheck memory. None of them can be rolled back.
      if (std::clamp<size_t>(Addr, StackSec.VMA,
                             StackSec.VMA + StackSec.Size) == Addr ||
          std::clamp<size_t>(Addr, SelfcheckSec.VMA,
                             SelfcheckSec.VMA + SelfcheckSec.Size) == Addr)
        continue;
      auto RP = SGCtx->getRegisterPool();
      SnippyTgt.storeValueToAddr(
          MBB, MBB.getFirstTerminator(), Addr,
          {sizeof(Data) * CHAR_BIT, static_cast<unsigned char>(Data)}, RP,
          *SGCtx);
    }
  }

  writeRegsSnapshot(FRegsSnapshot, MBB, RegStorageType::FReg, *SGCtx);
  writeRegsSnapshot(VRegsSnapshot, MBB, RegStorageType::VReg, *SGCtx);
  // Execute inserted instructions to get a change made by the opened
  // transaction.
  if (interpretInstrs(MBB.begin(), MBB.getFirstTerminator()) !=
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
  writeRegsSnapshot(WholeXRegsSnapshot, MBB, RegStorageType::XReg, *SGCtx);

  return GenerationStatistics(
      0, getCodeSize(MBB.begin(), MBB.getFirstTerminator()));
}

void InstructionGenerator::generate(
    planning::FunctionRequest &FunctionGenRequest, MachineFunction &MF) {
  GenerationStatistics CurrMFGenStats;

  auto &MLI = getAnalysis<MachineLoopInfo>();
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
    if (SGCtx->hasTrackingMode())
      ML = MLI.getLoopFor(MBB);

    if (ML && ML->getHeader() == MBB) {
      // Remember the current state (open transaction) before loop body
      // execution.
      assert(SGCtx->hasTrackingMode());
      auto &I = SGCtx->getOrCreateInterpreter();
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
      CurrMFGenStats.merge(generateCompensationCode(*MBB));
    } else {
      if (SGCtx->hasTrackingMode()) {
        if (interpretInstrs(MBB->begin(), MBB->getFirstTerminator()) !=
            GenerationStatus::Ok)
          report_fatal_error(
              "Interpretation failed on instructions inserted before "
              "main instructions generation routine.",
              false);
      }

      SNIPPY_DEBUG_BRIEF("request for reachable BasicBlock", BBReq);

      auto GeneratedStats = generate(BBReq, *MBB);
      CurrMFGenStats.merge(std::move(GeneratedStats));
    }
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);

    if (SGCtx->hasTrackingMode() && MBB == &MF.back()) {
      // The model has executed blocks from entry till exit. All other blocks
      // are dead and we'll handle then in the next loop below.
      break;
    }

    MBB = findNextBlock(MBB, NotVisited, ML);
    if (!ML || ML->getExitBlock() != MBB)
      continue;

    auto &I = SGCtx->getOrCreateInterpreter();
    // We are in exit block, it means that the loop was generated and we can
    // commit the transaction.
    I.commitTransaction();
    // Set expected values for registers that participate in the loop exit
    // condition (we execute only the first iteration of the loop). See
    // addIncomingValues/getIncomingValues description for additional details.
    for (const auto &[Reg, Value] : SGCtx->getIncomingValues(MBB))
      I.setReg(Reg, Value);
  }

  LLVM_DEBUG(
      dbgs() << "Instructions generation with enabled tracking on model: "
             << NotVisited.size() << " basic block are dead.\n");

  assert((NotVisited.empty() || SGCtx->hasTrackingMode()) &&
         "At this point some basic block might not be visited only when model "
         "controls generation routine.");

  if (SGCtx->hasTrackingMode()) {
    // Model must not be used further as it finished execution.
    SGCtx->disableTrackingMode();
    auto &I = SGCtx->getOrCreateInterpreter();
    I.disableTransactionsTracking();
    I.resetMem();
  }

  MBB = findNextBlock(nullptr, NotVisited);
  while (!FunctionGenRequest.isLimitReached(CurrMFGenStats)) {
    assert(MBB);
    assert(!NotVisited.empty() &&
           "There are no more block to insert instructions.");
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.at(MBB);
    SNIPPY_DEBUG_BRIEF("request for dead BasicBlock", BBReq);

    auto GeneratedStats = generate(BBReq, *MBB);
    CurrMFGenStats.merge(std::move(GeneratedStats));
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);
    MBB = findNextBlock(nullptr, NotVisited);
  }

  finalizeFunction(MF, FunctionGenRequest, CurrMFGenStats);
}

void InstructionGenerator::finalizeFunction(
    MachineFunction &MF, planning::FunctionRequest &Request,
    const GenerationStatistics &MFStats) {
  auto &State = SGCtx->getLLVMState();
  auto &MBB = MF.back();

  auto LastInstr = SGCtx->getLastInstr();
  bool NopLastInstr = LastInstr.empty();

  // Secondary functions always return.
  if (!SGCtx->isRootFunction(MF)) {
    State.getSnippyTarget().generateReturn(MBB, State);
    return;
  }

  // Root functions are connected via tail calls.
  if (!SGCtx->isExitFunction(MF)) {
    State.getSnippyTarget().generateTailCall(MBB, *SGCtx->nextRootFunction(MF),
                                             *SGCtx);
    return;
  }

  auto *ExitSym =
      MF.getContext().getOrCreateSymbol(Linker::GetExitSymbolName());

  // User may ask for last instruction to be return.
  if (SGCtx->useRetAsLastInstr()) {
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
    auto &Limit = FinalReq.limit();
    assert(Limit.getLimit() && "FinalReq is empty!");
    auto RP = SGCtx->getRegisterPool();
    GenerationStatistics Stats;
    generate(FinalReq, MF.back(), Stats);
  }

  // Mark last generated instruction as support one.
  setAsSupportInstr(*(--MBB.getFirstTerminator()), State.getCtx());
  MBB.back().setPreInstrSymbol(MF, ExitSym);
}

bool InstructionGenerator::runOnMachineFunction(MachineFunction &MF) {
  SGCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &GenSettings = SGCtx->getGenSettings();

  if (SGCtx->hasTrackingMode())
    prepareInterpreterEnv();

  if (GenSettings.TrackingConfig.BTMode)
    BT = createBacktrack();

  if (ExportGV)
    addModelMemoryPropertiesAsGV();

  if (GenSettings.TrackingConfig.SelfCheckPeriod) {
    SelfcheckPeriodTracker = {GenSettings.TrackingConfig.SelfCheckPeriod};
    const auto &SCSection = SGCtx->getSelfcheckSection();
    SCAddress = SCSection.VMA;
    // FIXME: make SelfCheckGV a deprecated option
    if (SelfCheckGV || ExportGV) {
      if (!ExportGV)
        addModelMemoryPropertiesAsGV();
      addSelfcheckSectionPropertiesAsGV();
    }
  }

  auto FunctionGenRequest = createMFGenerationRequest(MF);
  SNIPPY_DEBUG_BRIEF("request for function", FunctionGenRequest);

  generate(FunctionGenRequest, MF);
  return true;
}

static void dumpVerificationIntervalsIfNeeeded(StringRef Output,
                                               const GeneratorContext &GenCtx) {
  if (!RegionsToVerify.isSpecified())
    return;

  auto &State = GenCtx.getLLVMState();
  auto &Ctx = State.getCtx();

  auto VerificationIntervals = IntervalsToVerify::createFromObject(
      State.getDisassembler(), Output, GenCtx.getEntryPointName(),
      GenCtx.getLinker().getOutputSectionFor(".text").Desc.VMA,
      GenCtx.getEntryPrologueInstructionCount(),
      GenCtx.getEntryEpilogueInstructionCount());

  if (!VerificationIntervals)
    snippy::fatal(Ctx, "Failed to extract pc intervals to verify",
                  VerificationIntervals.takeError());

  const auto &GenSettings = GenCtx.getGenSettings();
  auto RegionsToVerifyFilename =
      RegionsToVerify.isSpecified() && RegionsToVerify.getValue().empty()
          ? addExtensionIfRequired(GenSettings.BaseFileName,
                                   ".intervals-to-verify.yaml")
          : RegionsToVerify.getValue();

  if (auto E = VerificationIntervals->dumpAsYaml(RegionsToVerifyFilename))
    snippy::fatal(Ctx, "Failed to save YAML to " + RegionsToVerifyFilename,
                  std::move(E));
}

static RegisterGenerator createRegGen(std::string PluginFileName,
                                      std::string InfoFileName) {
  if (PluginFileName == "None")
    PluginFileName = "";
  if (InfoFileName == "None")
    InfoFileName = "";
  return RegisterGenerator{PluginFileName, InfoFileName};
}

GeneratorResult FlowGenerator::generate(LLVMState &State) {
  auto &LLVMTM = State.getTargetMachine();
  auto &Ctx = State.getCtx();
  Module M("SnippyModule", Ctx);

  // Previously, AsmPrinter was created using Context from MMI
  // MMI is captured by PM, so in order to avoid potential invalid ref,
  //  now MMIWP uses external context
  MCContext Context(LLVMTM.getTargetTriple(), LLVMTM.getMCAsmInfo(),
                    LLVMTM.getMCRegisterInfo(), LLVMTM.getMCSubtargetInfo(),
                    nullptr, &LLVMTM.Options.MCOptions, false);
  Context.setObjectFileInfo(LLVMTM.getObjFileLowering());
  auto MMIWP =
      std::make_unique<MachineModuleInfoWrapperPass>(&LLVMTM, &Context);
  auto &MMI = MMIWP->getMMI();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto RegGen =
      createRegGen(RegGeneratorFile.getValue(), RegInfoFile.getValue());

  GeneratorContext GenCtx(MMI, M, State, RP, RegGen, GenSettings, OpCC);
  GenCtx.attachTargetContext(SnippyTgt.createTargetContext(GenCtx));

  PassManagerWrapper PM;
  initializeCodeGen(*PassRegistry::getPassRegistry());
  SnippyTgt.initializeTargetPasses();

  // Pre backtrack start
  PM.add(MMIWP.release());
  PM.add(createGeneratorContextWrapperPass(GenCtx));
  PM.add(createFunctionGeneratorPass());
  PM.add(createReserveRegsPass());
  PM.add(createCFGeneratorPass());
  if (GenSettings.Cfg.Branches.PermuteCF) {
    PM.add(createCFPermutationPass());
    if (GenSettings.DebugConfig.PrintControlFlowGraph)
      PM.add(createCFGPrinterPass());
    PM.add(createLoopAlignmentPass());
    PM.add(createLoopCanonicalizationPass());
    PM.add(createLoopLatcherPass());
    if (GenSettings.DebugConfig.PrintControlFlowGraph)
      PM.add(createCFGPrinterPass());
  }

  PM.add(
      createRegsInitInsertionPass(GenSettings.RegistersConfig.InitializeRegs));
  SnippyTgt.addTargetSpecificPasses(PM);

  // Pre backtrack end

  PM.add(createBlockGenPlanningPass());
  PM.add(createInstructionGeneratorPass()); // Can be backtracked

  if (GenSettings.InstrsGenerationConfig.RunMachineInstrVerifier)
    PM.add(createMachineVerifierPass("Machine Verifier Pass report"));

  // Post backtrack
  PM.add(createPrologueEpilogueInsertionPass());
  PM.add(createFillExternalFunctionsStubsPass({}));
  if (GenSettings.DebugConfig.PrintMachineFunctions)
    PM.add(createMachineFunctionPrinterPass(outs()));

  if (GenSettings.DebugConfig.PrintControlFlowGraph)
    PM.add(createCFGPrinterPass());
  if (GenSettings.DebugConfig.PrintInstrs)
    PM.add(createPrintMachineInstrsPass(outs()));

  SnippyTgt.addTargetLegalizationPasses(PM);

  PM.add(createBranchRelaxatorPass());
  if (VerifyConsecutiveLoops)
    PM.add(createConsecutiveLoopsVerifierPass());

  PM.add(createInstructionsPostProcessPass());
  PM.add(createFunctionDistributePass());
  std::string MIR;
  raw_string_ostream MIROS(MIR);
  if (DumpMIR.isSpecified())
    PM.add(createPrintMIRPass(MIROS));

  SmallString<32> Output;
  raw_svector_ostream OS(Output);
  PM.addAsmPrinter(LLVMTM, OS, nullptr, CodeGenFileType::CGFT_ObjectFile,
                   Context);

  PM.run(M);

  outs().flush(); // FIXME: this is currently needed because
                  //        MachineFunctionPrinter don't flush
  auto CGFilename = DumpCGFilename.getValue();
  if (!CGFilename.empty())
    GenCtx.getCallGraphState().dump(CGFilename, CGDumpFormat);

  if (DumpMIR.isSpecified())
    writeMIRFile(MIR);
  dumpMemAccessesIfNeeded(GenCtx);
  auto ImagesForFinalSnippet = ObjectFilesList{Output};

  auto Result = GenCtx.generateELF(ImagesForFinalSnippet);

  auto ImagesForModelExecution = ImagesForFinalSnippet;

  auto SnippetImageForModelExecution =
      GenCtx.generateLinkedImage(ImagesForModelExecution);

  dumpVerificationIntervalsIfNeeeded(Output, GenCtx);

  GenCtx.runSimulator(SnippetImageForModelExecution);

  if (!Result.LinkerScript.empty())
    snippy::notice(
        WarningName::RelocatableGenerated, State.getCtx(),
        "Snippet generator generated relocatable image",
        "please, use linker with provided script to generate final image");

  return Result;
}

} // namespace snippy
} // namespace llvm
