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
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/IntervalsToVerify.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
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

static snippy::opt<unsigned> BurstAddressRandomizationThreshold(
    "burst-addr-rand-threshold",
    cl::desc(
        "Number of attempts to randomize address of each instruction in the "
        "burst group"),
    cl::Hidden, cl::init(100));

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
static snippy::opt<std::string> DumpMemAccesses(
    "dump-memory-accesses",
    cl::desc("Dump memory addresses that are accessed by instructions in the "
             "snippet to a file creating `access-addresses` memory scheme. It "
             "does not record auxiliary instructions (e.g. selfcheck). Pass "
             "the options with an empty value to dump to stdout."),
    cl::value_desc("filename"), cl::cat(Options));
static snippy::opt<std::string> DumpMemAccessesRestricted(
    "dump-memory-accesses-restricted",
    cl::desc(
        "Dump memory addresses that are accessed by instructions in the "
        "snippet to a file creating `restricted-addresses` memory scheme. It "
        "does not record auxiliary instructions (e.g. selfcheck). Pass "
        "the options with an empty value to dump to stdout."),
    cl::value_desc("filename"), cl::cat(Options));

static snippy::opt<bool> DumpBurstPlainAccesses(
    "dump-burst-accesses-as-plain-addrs",
    cl::desc("Dump memory addresses accessed by burst groups as plain "
             "addresses rather than ranges descriptions"),
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

static bool dumpPlainAccesses(std::stringstream &SS,
                              const PlainAccessesType &Accesses,
                              bool Restricted, StringRef Prefix, bool Append) {
  if (Accesses.empty())
    return false;

  if (!Append)
    SS << Prefix.data() << "plain:\n";

  for (auto [Addr, AccessSize] : Accesses) {
    SS << "      - addr: 0x" << Twine::utohexstr(Addr).str() << "\n";
    if (Restricted)
      SS << "        access-size: " << AccessSize << "\n";
  }
  return true;
}

static void dumpBurstAccesses(std::stringstream &SS,
                              const BurstGroupAccessesType &Accesses,
                              StringRef Prefix) {
  if (Accesses.empty())
    return;

  SS << Prefix.data() << "burst:\n";

  for (const auto &BurstDesc : Accesses) {
    assert(!BurstDesc.empty() && "At least one range is expected");
    for (auto &RangeDesc : BurstDesc) {
      assert(RangeDesc.MinOffset <= RangeDesc.MaxOffset);
      bool IsMinNegative = RangeDesc.MinOffset < 0;
      MemAddr MinOffsetAbs = std::abs(RangeDesc.MinOffset);
      bool IsMaxNegative = RangeDesc.MaxOffset < 0;
      MemAddr MaxOffsetAbs = std::abs(RangeDesc.MaxOffset);
      assert(!IsMinNegative || RangeDesc.Address >= MinOffsetAbs);
      assert(IsMinNegative ||
             std::numeric_limits<MemAddr>::max() - RangeDesc.Address >=
                 MinOffsetAbs);
      auto BaseAddr = IsMinNegative ? RangeDesc.Address - MinOffsetAbs
                                    : RangeDesc.Address + MinOffsetAbs;
      MemAddr Size = 0;
      if (IsMaxNegative) {
        assert(MinOffsetAbs >= MaxOffsetAbs);
        Size = MinOffsetAbs - MaxOffsetAbs;
      } else if (IsMinNegative) {
        assert(std::numeric_limits<MemAddr>::max() - MaxOffsetAbs >=
               MinOffsetAbs);
        Size = MaxOffsetAbs + MinOffsetAbs;
      } else {
        assert(MaxOffsetAbs >= MinOffsetAbs);
        Size = MaxOffsetAbs - MinOffsetAbs;
      }
      assert(std::numeric_limits<MemAddr>::max() - Size >=
             RangeDesc.AccessSize);
      Size += RangeDesc.AccessSize;
      SS << "        - addr: 0x" << Twine::utohexstr(BaseAddr).str() << "\n";
      SS << "          size: " << Size << "\n";
      SS << "          stride: " << RangeDesc.MinStride << "\n";
      SS << "          access-size: " << RangeDesc.AccessSize << "\n";
    }
  }
}

static void dumpMemAccesses(StringRef Filename, const PlainAccessesType &Plain,
                            const BurstGroupAccessesType &BurstRanges,
                            const PlainAccessesType &BurstPlain,
                            bool Restricted) {
  if (Plain.empty() && BurstRanges.empty() && BurstPlain.empty()) {
    errs() << "warning: cannot dump memory accesses as no accesses were "
              "generated. File won't be created.\n";
    return;
  }

  std::stringstream SS;
  SS << (Restricted ? "restricted-addresses:\n" : "access-addresses:\n");
  if (!Restricted)
    SS << "  - ordered: true\n";

  constexpr auto NewEntry = "  - ";
  constexpr auto SameEntry = "    ";
  auto Prefix = Restricted ? NewEntry : SameEntry;

  auto Added =
      dumpPlainAccesses(SS, Plain, Restricted, Prefix, /* Append */ false);
  if (Added)
    Prefix = SameEntry;

  if (!Restricted && !DumpBurstPlainAccesses)
    dumpBurstAccesses(SS, BurstRanges, Prefix);
  else
    dumpPlainAccesses(SS, BurstPlain, Restricted, Prefix, /* Append */ Added);

  if (Filename.empty())
    outs() << SS.str();
  else
    writeFile(Filename, SS.str());
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

static auto getEffectiveMemoryAccessInfo(const MachineInstr *MI, AddressInfo AI,
                                         const SnippyTarget &SnippyTgt) {
  MemAddr EffectiveAddr = AI.Address;
  auto Offset =
      find_if(MI->operands(), [](const auto &Op) { return Op.isImm(); });
  // Some instructions don't have offset operand.
  if (Offset != MI->operands().end())
    EffectiveAddr += Offset->getImm();
  return std::make_pair(EffectiveAddr,
                        SnippyTgt.getAccessSize(MI->getOpcode()));
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

// Helper class to keep additional information about the operand: register
// number that has been somehow selected before instruction generation,
// immediate operand range and so on.
class PreselectedOpInfo {
  using EmptyTy = std::monostate;
  using RegTy = llvm::Register;
  using ImmTy = StridedImmediate;
  using TiedTy = int;
  std::variant<EmptyTy, RegTy, ImmTy, TiedTy> Value;

  unsigned Flags = 0;

public:
  PreselectedOpInfo(llvm::Register R) : Value(R) {}
  PreselectedOpInfo(StridedImmediate Imm) : Value(Imm) {}
  PreselectedOpInfo() = default;
  bool isReg() const { return std::holds_alternative<RegTy>(Value); }
  bool isImm() const { return std::holds_alternative<ImmTy>(Value); }
  bool isUnset() const { return std::holds_alternative<EmptyTy>(Value); }
  bool isTiedTo() const { return std::holds_alternative<TiedTy>(Value); }

  unsigned getFlags() const { return Flags; }
  StridedImmediate getImm() const {
    assert(isImm());
    return std::get<ImmTy>(Value);
  }
  llvm::Register getReg() const {
    assert(isReg());
    return std::get<RegTy>(Value);
  }
  llvm::Register getTiedTo() const {
    assert(isTiedTo());
    return std::get<TiedTy>(Value);
  }

  void setFlags(unsigned F) { Flags = F; }
  void setTiedTo(int OpIdx) {
    assert(isUnset());
    Value = OpIdx;
  }
};

class InstructionGenerator final : public MachineFunctionPass {

  GenerationStatistics processInstructionGeneration(
      MachineBasicBlock &MBB, IInstrGroupGenReq &InstructionGroupRequest,
      GenerationStatistics &CurrentInstructionGroupStats);

  GenerationResult
  generateNopsToSizeLimit(MachineBasicBlock &MBB,
                          const IInstrGroupGenReq &InstructionGroupRequest,
                          GenerationStatistics &CurrentInstructionGroupStats);

  void generateInstruction(MachineBasicBlock &MBB, unsigned Opc,
                           MachineBasicBlock::iterator Ins);

  template <typename InstrIt>
  GenerationStatus interpretInstrs(InstrIt Begin, InstrIt End) const;

  template <typename InstrIt>
  std::vector<InstrIt> collectSelfcheckCandidates(InstrIt Begin,
                                                  InstrIt End) const;

  template <typename InstrIt>
  GenerationResult handleGeneratedInstructions(
      MachineBasicBlock &MBB, InstrIt ItBegin, InstrIt ItEnd,
      const IInstrGroupGenReq &InstructionGroupRequest,
      const GenerationStatistics &CurrentInstructionGroupStats) const;

  void selfcheckOverflowGuard() const;

  // Returns iterator to the first inserted instruction. So [returned It;
  // InsertPos) contains inserted instructions.
  // Register pool is taken by value as we'll do some local reservations on top
  // of the existing register pool. All changes made must be canceled at the
  // function exit.
  template <typename InstrIt>
  SelfcheckIntermediateInfo<InstrIt>
  storeRefAndActualValueForSelfcheck(MachineBasicBlock &MBB, InstrIt InsertPos,
                                     Register DestReg, RegPoolWrapper RP) const;

  MachineInstr *
  randomInstruction(const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
                    MachineBasicBlock::iterator Ins,
                    std::vector<PreselectedOpInfo> Preselected = {}) const;

  GenerationStatistics
  generateInstrGroup(MachineBasicBlock &MBB,
                     IInstrGroupGenReq &InstructionGroupRequest);

  GenerationStatistics generateForMBB(MachineBasicBlock &MBB,
                                      IMBBGenReq &Request);

  MachineBasicBlock *
  findNextBlock(const MachineBasicBlock *MBB,
                const std::set<MachineBasicBlock *, MIRComp> &NotVisited,
                const MachineLoop *ML = nullptr) const;

  GenerationStatistics generateCompensationCode(MachineBasicBlock &MBB);

  MachineBasicBlock *findNextBlockOnModel(const MachineBasicBlock &MBB) const;

  std::unique_ptr<IFunctionGenReq>
  createMFGenerationRequest(const MachineFunction &MF) const;

  void generateForFunction(MachineFunction &MF, IFunctionGenReq &Request);

  void finalizeFunction(MachineFunction &MF, IFunctionGenReq &Request,
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

  void generateBurst(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                     IInstrGroupGenReq &BurstGroupRequest) const;

  // Attempt to generate valid operands
  std::optional<SmallVector<MachineOperand, 8>> tryToPregenerateOperands(
      const MachineInstrBuilder &MIB, const MCInstrDesc &InstrDesc,
      RegPoolWrapper &RP,
      const std::vector<PreselectedOpInfo> &Preselected) const;

  // In order to support register plugin InstructionGenerator pre-generates all
  //  instructio operands.
  // Random generator always generates valid registers,
  //  but plugin may abort this process on an intermediate operand.
  // In this case InstructionGenerator starts generation from the first operand
  //  of the current instruction.
  SmallVector<MachineOperand, 8>
  pregenerateOperands(const MachineInstrBuilder &MIB,
                      const MachineBasicBlock &MBB,
                      const MCInstrDesc &InstrDesc, RegPoolWrapper &RP,
                      const std::vector<PreselectedOpInfo> &Preselected) const;

  // Generates operand and stores it into the queue.
  std::optional<MachineOperand>
  pregenerateOneOperand(const MachineInstrBuilder &MIB, RegPoolWrapper &RP,
                        const MCOperandInfo &MCOpInfo,
                        const PreselectedOpInfo &Preselected,
                        unsigned OperandIndex,
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

void GenerationStatistics::merge(const GenerationResult &Res) {
  merge(Res.Stats);
}

InstructionGenerator::InstructionGenerator() : MachineFunctionPass(ID) {
  initializeInstructionGeneratorPass(*PassRegistry::getPassRegistry());
}

void InstructionGenerator::generateInstruction(
    MachineBasicBlock &MBB, unsigned Opc, MachineBasicBlock::iterator Ins) {
  auto &State = SGCtx->getLLVMState();
  const auto &InstrDesc = State.getInstrInfo().get(Opc);
  const auto &SnippyTgt = State.getSnippyTarget();

  assert(!InstrDesc.isBranch() &&
         "All branch instructions expected to be generated separately");

  if (GenerateInsertionPointHints)
    generateInsertionPointHint(MBB, MBB.end());

  if (SnippyTgt.requiresCustomGeneration(InstrDesc)) {
    SnippyTgt.generateCustomInst(InstrDesc, MBB, *SGCtx, Ins);
    return;
  }

  if (SnippyTgt.isCall(Opc))
    generateCall(MBB, Ins, Opc);
  else
    randomInstruction(InstrDesc, MBB, Ins);
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
    if (!I.step())
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

template <typename InstrIt>
GenerationResult InstructionGenerator::handleGeneratedInstructions(
    MachineBasicBlock &MBB, InstrIt ItBegin, InstrIt ItEnd,
    const IInstrGroupGenReq &InstructionGroupRequest,
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
  if (!InstructionGroupRequest.canGenerateMore(CurrentInstructionGroupStats,
                                               GeneratedCodeSize))
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
  I.executeChainOfInstrs(SGCtx->getLLVMState(), InsPoint, ItEnd);

  // Check size requirements after selfcheck addition.
  GeneratedCodeSize = getCodeSize(ItBegin, ItEnd);
  if (!InstructionGroupRequest.canGenerateMore(CurrentInstructionGroupStats,
                                               GeneratedCodeSize))
    return ReportGenerationResult(GenerationStatus::SizeFailed);
  return ReportGenerationResult(GenerationStatus::Ok);
}

GenerationResult InstructionGenerator::generateNopsToSizeLimit(
    MachineBasicBlock &MBB, const IInstrGroupGenReq &InstructionGroupRequest,
    GenerationStatistics &CurrentInstructionGroupStats) {
  assert(InstructionGroupRequest.getGenerationLimit(GenerationMode::Size));
  auto &State = SGCtx->getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto ItEnd = MBB.getFirstTerminator();
  auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);
  while (!InstructionGroupRequest.isCompleted(CurrentInstructionGroupStats)) {
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

GenerationStatistics InstructionGenerator::processInstructionGeneration(
    MachineBasicBlock &MBB, IInstrGroupGenReq &InstructionGroupRequest,
    GenerationStatistics &CurrentInstructionGroupStats) {
  // ensure that there is no underflow
  assert(!InstructionGroupRequest.isCompleted(CurrentInstructionGroupStats));

  auto BacktrackCount = 0u;
  auto SizeErrorCount = 0u;

  for (;;) {
    auto ItEnd = MBB.getFirstTerminator();
    auto ItBegin = ItEnd == MBB.begin() ? ItEnd : std::prev(ItEnd);

    std::optional<unsigned> Opc = std::nullopt;
    if (InstructionGroupRequest.getBurstGroupID()) {
      generateBurst(MBB, ItEnd, InstructionGroupRequest);
    } else {
      Opc = InstructionGroupRequest.genOpc();
      generateInstruction(MBB, Opc.value(), ItEnd);
    }

    ItBegin = ItBegin == ItEnd ? MBB.begin() : std::next(ItBegin);
    auto IntRes = handleGeneratedInstructions(MBB, ItBegin, ItEnd,
                                              InstructionGroupRequest,
                                              CurrentInstructionGroupStats);

    LLVM_DEBUG(printInterpretResult(dbgs(), "      ", IntRes); dbgs() << "\n");

    switch (IntRes.Status) {
    case GenerationStatus::InterpretFailed: {
      report_fatal_error(
          "Fail to execute generated instruction in tracking mode", false);
    }
    case GenerationStatus::BacktrackingFailed: {
      ++BacktrackCount;
      if (BacktrackCount < BTThreshold)
        continue;
      report_fatal_error("Back-tracking events threshold is exceeded", false);
    }
    case GenerationStatus::SizeFailed: {
      ++SizeErrorCount;
      if (SizeErrorCount < SizeErrorThreshold)
        continue;

      // We have size error too many times, let's generate nops up to size
      // limit
      auto GenRes = generateNopsToSizeLimit(MBB, InstructionGroupRequest,
                                            CurrentInstructionGroupStats);
      if (GenRes.Status != GenerationStatus::Ok)
        report_fatal_error("Nop generation during block fill by size failed",
                           false);
      return GenerationStatistics(GenRes.Stats);
    }
    case GenerationStatus::Ok: {
      auto &SnpTgt = SGCtx->getLLVMState().getSnippyTarget();
      if (Opc.has_value() && SnpTgt.needsGenerationPolicySwitch(Opc.value()))
        InstructionGroupRequest.changePolicy(
            SnpTgt.getGenerationPolicy(MBB, *SGCtx, std::nullopt));
      break;
    }
    }

    return GenerationStatistics(IntRes.Stats);
  }
}

// Count how many def regs of a register class RC the instruction has.
static unsigned countDefsHavingRC(ArrayRef<unsigned> Opcodes,
                                  const TargetRegisterInfo &RegInfo,
                                  const TargetRegisterClass &RC,
                                  const MCInstrInfo &InstrInfo) {
  auto CountDefsForOpcode = [&](unsigned Init, unsigned Opcode) {
    const auto &InstrDesc = InstrInfo.get(Opcode);
    auto NumDefs = InstrDesc.getNumDefs();
    auto DefBegin = InstrDesc.operands().begin();
    auto DefEnd = std::next(DefBegin, NumDefs);
    return Init +
           std::count_if(DefBegin, DefEnd, [&](const MCOperandInfo &OpInfo) {
             const auto *OpRC = RegInfo.getRegClass(OpInfo.RegClass);
             return RC.hasSubClassEq(OpRC);
           });
  };
  return std::accumulate(Opcodes.begin(), Opcodes.end(), 0u,
                         CountDefsForOpcode);
}

static unsigned countAddrs(ArrayRef<unsigned> Opcodes,
                           const SnippyTarget &SnippyTgt) {
  auto CountAddrsForOpcode = [&SnippyTgt](unsigned Init, unsigned Opcode) {
    return Init + SnippyTgt.countAddrsToGenerate(Opcode);
  };
  return std::accumulate(Opcodes.begin(), Opcodes.end(), 0u,
                         CountAddrsForOpcode);
}

// NumDefs + NumAddrs might be more than a number of available regs. This
// normalizes the number of regs to reserve for addrs.
static unsigned normalizeNumRegs(unsigned NumDefs, unsigned NumAddrs,
                                 unsigned NumRegs) {
  if (NumRegs == 0)
    report_fatal_error("No registers left to reserve for burst mode", false);
  auto Ratio = 1.0 * NumRegs / (NumAddrs + NumDefs);
  if (Ratio > 1.0)
    return NumAddrs;
  unsigned NumAddrRegsToGen = Ratio * NumAddrs;
  assert(NumAddrRegsToGen + Ratio * NumDefs <= NumRegs &&
         "Wrong number of registers to reserve");
  return NumAddrRegsToGen;
}

// For the given InstrDesc fill the vector of selected operands to account them
// in instruction generation procedure.
static auto selectOperands(const MCInstrDesc &InstrDesc, unsigned BaseReg,
                           const AddressInfo &AI) {
  std::vector<PreselectedOpInfo> Preselected;
  for (const auto &MCOpInfo : InstrDesc.operands()) {
    if (MCOpInfo.OperandType == MCOI::OperandType::OPERAND_MEMORY)
      Preselected.emplace_back(BaseReg);
    else if (MCOpInfo.OperandType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
      // FIXME: Here we just use the fact that RISC-V loads and stores from base
      // subset have only one target specific operand that is offset.
      auto MinStride = AI.MinStride;
      if (MinStride == 0)
        MinStride = 1;
      assert(isPowerOf2_64(MinStride));
      Preselected.emplace_back(
          StridedImmediate(AI.MinOffset, AI.MaxOffset, MinStride));
    } else
      Preselected.emplace_back();
  }
  return Preselected;
}

static auto collectAddressRestrictions(ArrayRef<unsigned> Opcodes,
                                       GeneratorContext &GC,
                                       const MachineBasicBlock &MBB) {
  std::map<unsigned, AddressRestriction> OpcodeToAR;
  const auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  for (auto Opcode : Opcodes) {
    const auto &InstrDesc = InstrInfo.get(Opcode);
    if (!SnippyTgt.canUseInMemoryBurstMode(Opcode))
      continue;

    // Get address restrictions for the current opcode.
    AddressRestriction AR;
    AR.Opcodes.insert(Opcode);
    std::tie(AR.AccessSize, AR.AccessAlignment) =
        SnippyTgt.getAccessSizeAndAlignment(Opcode, GC, MBB);
    AR.ImmOffsetRange = SnippyTgt.getImmOffsetRangeForMemAccessInst(InstrDesc);

    assert(!OpcodeToAR.count(Opcode) ||
           OpcodeToAR[Opcode].ImmOffsetRange == AR.ImmOffsetRange);
    OpcodeToAR.try_emplace(Opcode, AR);
  }
  return OpcodeToAR;
}

static auto deduceStrongestRestrictions(
    ArrayRef<unsigned> Opcodes, ArrayRef<unsigned> OpcodeIdxToBaseReg,
    const std::map<unsigned, AddressRestriction> &OpcodeToAR) {
  assert(Opcodes.size() == OpcodeIdxToBaseReg.size());
  assert(all_of(Opcodes, [&OpcodeToAR](auto Opcode) {
    return OpcodeToAR.count(Opcode);
  }));
  std::map<unsigned, std::set<unsigned>> BaseRegToOpcodes;
  for (auto [OpcodeIdx, BaseReg] : enumerate(OpcodeIdxToBaseReg))
    BaseRegToOpcodes[BaseReg].insert(Opcodes[OpcodeIdx]);

  std::map<unsigned, AddressRestriction> BaseRegToAR;
  for (const auto &[BaseReg, Opcodes] : BaseRegToOpcodes) {
    auto StrongestAccessSize = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).AccessSize < OpcodeToAR.at(RHS).AccessSize;
        });
    auto StrongestImmMin = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getMin() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getMin();
        });
    auto StrongestImmMax = std::min_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getMax() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getMax();
        });
    auto StrongestImmStride = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getStride() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getStride();
        });

    AddressRestriction StrongestAR;

    StrongestAR.AccessSize = OpcodeToAR.at(*StrongestAccessSize).AccessSize;
    StrongestAR.AccessAlignment =
        OpcodeToAR.at(*StrongestAccessSize).AccessAlignment;

    StrongestAR.ImmOffsetRange = StridedImmediate(
        OpcodeToAR.at(*StrongestImmMin).ImmOffsetRange.getMin(),
        OpcodeToAR.at(*StrongestImmMax).ImmOffsetRange.getMax(),
        OpcodeToAR.at(*StrongestImmStride).ImmOffsetRange.getStride());

    // We insert all opcodes because the address chosen for this restriction
    // will be used as a fallback if we fail to find another one.
    StrongestAR.Opcodes.insert(Opcodes.begin(), Opcodes.end());

    BaseRegToAR[BaseReg] = std::move(StrongestAR);
  }

  return BaseRegToAR;
}

// Memory schemes return random address with such offsets that they include zero
// offset. So, when memory scheme is restrictive, for example has small size, we
// can generate only small immediate offsets. For example for the scheme below
//
//   access-ranges:
//      - start: 0x800FFFF0
//        size: 0x10
//        stride: 1
//        first-offset: 0
//        last-offset: 0
//
// Memory scheme may return the following address infos:
//
//  Base Addr    MinOff  MaxOff
// [0x800FFFF0,    0,     15]
// [0x800FFFF1,   -1,     14]
// [0x800FFFF2,   -2,     13]
// ...
// [0x800FFFFE,   -14,     1]
// [0x800FFFFF,   -15,     0]
//
// Such offsets may not cover whole possible range of ImmOffset field in
// instruction encoding (RISC-V in general allows 12-bit signed immediate).
//
// To make randomization better we generate a random shift for the offsets such
// that they will cover a part of imm range but not necessary contain zero
// offset. For example,
//   AddressInfo holds [0x800FFFF2, -2, 13],
//   immediate field allows immediates [-2048, 2047].
// Then we calculate random shift in range
//   [-2048 - (-2), 2047 - 13] => [-2046, 2034].
// Let's say randomly chosen value is -1000, we apply it to original range from
// address info:
//   [0x800FFFF2, -2, 13] => [0x800FFFF2 - (-1000), -2 + (-1000), 13 + (-1000)]
//     => [0x801003DA, -1002, -987].
// As you can check, all possible addresses (base addr + offset) match for both
// ranges.
//
// If the legal values for an immediate aren't an interval (which is the case
// for RISC-V compressed loads and stores), the random shift must be generated
// with some kind of stride.
static AddressInfo
randomlyShiftAddressOffsetsInImmRange(AddressInfo AI,
                                      StridedImmediate ImmRange) {
  auto MinImm = ImmRange.getMin();
  auto MaxImm = ImmRange.getMax();
  assert(MinImm <= 0 && 0 <= MaxImm);
  assert(ImmRange.getStride() == 0 || MinImm % ImmRange.getStride() == 0);
  assert(AI.MinOffset <= 0 && 0 <= AI.MaxOffset);

  assert(ImmRange.getStride() <= std::numeric_limits<int64_t>::max() &&
         "Unexpected stride for immediate");
  assert(AI.MinStride <= std::numeric_limits<int64_t>::max() &&
         "Unexpected stride for AI");
  auto Stride = std::max<int64_t>(ImmRange.getStride(), AI.MinStride);
  Stride = std::max<int64_t>(Stride, 1);

  // Address info might be less restrictive than the immediate operand. For
  // example, legal final address can be aligned to 4, but the immediate operand
  // must be aligned to 8. So, when choosing legal immediate range, we must
  // account such restrictions.
  AI.MinOffset = alignTo(AI.MinOffset, Stride);
  AI.MaxOffset = alignDown(AI.MaxOffset, Stride);
  assert(AI.MinOffset <= 0 && 0 <= AI.MaxOffset);

  if (!(AI.MinOffset < MinImm && MaxImm < AI.MaxOffset)) {
    auto Shift =
        Stride * RandEngine::genInInterval<int64_t>(
                     std::min<int64_t>((MinImm - AI.MinOffset) / Stride, 0),
                     std::max<int64_t>((MaxImm - AI.MaxOffset) / Stride, 0));
    AI.MinOffset += Shift;
    AI.MaxOffset += Shift;
    AI.Address -= Shift;
  }

  AI.MinStride = Stride;
  return AI;
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

static std::vector<unsigned> generateBaseRegs(MachineBasicBlock &MBB,
                                              ArrayRef<unsigned> Opcodes,
                                              RegPoolWrapper &RP,
                                              GeneratorContext &SGCtx) {
  if (Opcodes.empty())
    return {};
  auto &State = SGCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  // Compute set of registers compatible with all opcodes
  std::unordered_set<unsigned> Exclude;
  for (auto Opcode : Opcodes) {
    copy(SnippyTgt.excludeFromMemRegsForOpcode(Opcode),
         std::inserter(Exclude, Exclude.begin()));
  }
  // Current implementation expects that each target has only one addr reg
  // class.
  const auto &AddrRegClass = SnippyTgt.getAddrRegClass();
  SmallVector<Register, 32> Include;
  copy_if(AddrRegClass, std::back_inserter(Include),
          [&](Register Reg) { return !Exclude.count(Reg); });

  // Normalize the number of addr registers to use. It's possible that we'll
  // re-use addr regs with different offset values.
  // FIXME: normalization does not account restrictions from memory schemes:
  // choosen number of base registers might not be enough.
  auto NumAvailRegs = RP.getNumAvailableInSet(Include, MBB);
  if (NumAvailRegs > 0 && SGCtx.getGenSettings().TrackingConfig.AddressVH) {
    // When hazard mode is enabled we'll likely need a register to transform
    // existing addresses.
    --NumAvailRegs;
  }
  if (NumAvailRegs == 0)
    report_fatal_error(
        "No available registers to generate addresses for the burst group.",
        false);
  const auto &RegInfo = *SGCtx.getSubtargetImpl().getRegisterInfo();
  // Get number of def and addr regs to use in the burst group. These values
  // can be bigger than the number of available registers.
  auto NumDefs = countDefsHavingRC(Opcodes, RegInfo, AddrRegClass, InstrInfo);
  auto NumAddrs = countAddrs(Opcodes, SnippyTgt);
  NumAddrs = normalizeNumRegs(NumDefs, NumAddrs, NumAvailRegs);

  // Randomly pick and reserve addr registers so as not to use them
  // destinations.
  auto AddrRegs = RP.getNAvailableRegisters(
      "for memory access burst", RegInfo, *AddrRegClass.MC, MBB, NumAddrs,
      [&](Register R) { return Exclude.count(R); });

  for (auto Reg : AddrRegs)
    RP.addReserved(Reg, AccessMaskBit::W);

  // We must be sure that each memory access in the burst group won't contradict
  // given memory schemes. To do that we 'attach' base address register from the
  // chosen above to each opcode (`OpcodeIdxToBaseReg`). After that we will know
  // a group of opcodes for each base register. Then for each group of opcodes
  // we collect restrictions on memory addresses such as stride and access size
  // (`BaseRegToAI` as we've already had a mapping from opcode index to the base
  // register). Gathered information gives us restriction on the offset
  // immediate for each base register for each opcode.
  std::vector<unsigned> OpcodeIdxToBaseReg(Opcodes.size());
  std::generate(OpcodeIdxToBaseReg.begin(), OpcodeIdxToBaseReg.end(),
                [&AddrRegs]() {
                  auto N = RandEngine::genInRange(AddrRegs.size());
                  return AddrRegs[N];
                });
  return OpcodeIdxToBaseReg;
}

// For the given AddressInfo and AddressRestriction try to find another
// AddressInfo such that:
// 1. New AddressInfo will cover different set of addresses (for better
// addresses and offsets randomization) in burst groups
// 2. Base address from the given AddressInfo may be reused for the new one. It
// means that the provided AddressRestriction allows immediate offsets that are
// necessary for the base address change.
//
// Example:
// Given AddressInfo is: base address = 10, min offset = 0, max offset = 0 -->
// effective address is 10 Given AddressRestriction on immediate offsets are:
// min = -5, max = 5
//
// We are limited in the number of registers we can use for base addresses in
// burst groups, but the given AddressInfo isn't really interesting: it doesn't
// allow immediate offsets other than zero. So, we'd like to choose another
// address info that provides wider range of immediate offsets and can use the
// original base address under the given AddressRestrictions.
//
// Let's say we've choosen a new AddressInfo: base address = 15, min offset =
// -10, max offset = 10 --> effective addresses are [5, 25] Replacement of the
// base address gives: base address = 10, min offset = -5 , max offset = 15 -->
// effective addresses are still the same [5, 25] Then we need to apply
// AddressRestrictions: base address = 10, min offset = -5 , max offset = 5 -->
// effective addresses were shrinked to [5, 15]
static AddressInfo
selectAddressForSingleInstrFromBurstGroup(AddressInfo OrigAI,
                                          const AddressRestriction &OpcodeAR,
                                          GeneratorContext &GC) {
  if (OrigAI.MinOffset != 0 || OrigAI.MaxOffset != 0 ||
      (OpcodeAR.ImmOffsetRange.getMin() == 0 &&
       OpcodeAR.ImmOffsetRange.getMax() == 0)) {
    // Either OrigAI allows non-zero offets, or address restrictions for the
    // given opcode doesn't allow non-zero offsets. In both cases there is
    // nothing to change.
    return OrigAI;
  }

  auto &MS = GC.getMemoryScheme();
  auto OrigAddr = OrigAI.Address;
  assert(OpcodeAR.Opcodes.size() == 1 &&
         "Expected AddressRestriction only for one opcode");
  for (unsigned i = 0; i < BurstAddressRandomizationThreshold; ++i) {
    auto CandidateAI =
        MS.randomAddress(OpcodeAR.AccessSize, OpcodeAR.AccessAlignment, false);

    auto Stride = std::max<int64_t>(OpcodeAR.ImmOffsetRange.getStride(),
                                    CandidateAI.MinStride);
    CandidateAI.MinStride = std::max<int64_t>(Stride, 1);

    if (CandidateAI.Address == OrigAddr && CandidateAI.MinOffset == 0 &&
        CandidateAI.MaxOffset == 0)
      continue;

    bool IsDiffNeg = OrigAddr >= CandidateAI.Address;
    auto AbsDiff = IsDiffNeg ? OrigAddr - CandidateAI.Address
                             : CandidateAI.Address - OrigAddr;
    auto SMax = std::numeric_limits<decltype(CandidateAI.MinOffset)>::max();
    auto SMin = std::numeric_limits<decltype(CandidateAI.MinOffset)>::min();
    assert(SMax > 0);
    // We are going to apply Diff to the signed type. Check that it fits.
    if (!IsDiffNeg && AbsDiff > static_cast<decltype(AbsDiff)>(SMax))
      continue;
    else if (IsDiffNeg &&
             AbsDiff > static_cast<decltype(AbsDiff)>(std::abs(SMin + 1)))
      continue;

    auto SDiff = static_cast<decltype(CandidateAI.MinOffset)>(AbsDiff);
    if (IsDiffNeg)
      SDiff = -SDiff;

    if (IsSAddOverflow(SDiff, CandidateAI.MinOffset) ||
        IsSAddOverflow(SDiff, CandidateAI.MaxOffset))
      continue;

    auto AlignedMinOffset =
        alignSignedTo(CandidateAI.MinOffset + SDiff, CandidateAI.MinStride);
    auto AlignedMaxOffset =
        alignSignedDown(CandidateAI.MaxOffset + SDiff, CandidateAI.MinStride);
    if (AlignedMaxOffset < AlignedMinOffset ||
        (AlignedMinOffset == 0 && AlignedMaxOffset == 0))
      continue;

    if (OpcodeAR.ImmOffsetRange.getMin() <= AlignedMinOffset &&
        AlignedMinOffset <= OpcodeAR.ImmOffsetRange.getMax() &&
        AlignedMinOffset <= AlignedMaxOffset &&
        (CandidateAI.Address + CandidateAI.MinOffset) % CandidateAI.MinStride ==
            (OrigAddr + AlignedMinOffset) % CandidateAI.MinStride) {
      CandidateAI.Address = OrigAddr;
      CandidateAI.MinOffset = AlignedMinOffset;
      CandidateAI.MaxOffset = AlignedMaxOffset;
      return CandidateAI;
    }
  }
  return OrigAI;
}

// Collect addresses that will meet the specified restrictions. We call such
// addresses "primary" because they'll be used as a defaults for the given base
// registers (set of opcodes mapped to the base register). Snippy will try to
// randomize addresses in a way that not only primary addresses are accessed
// (see selectAddressForSingleInstrFromBurstGroup), but base register is always
// taken suitable for the primary address.
static auto collectPrimaryAddresses(
    const std::map<unsigned, AddressRestriction> &BaseRegToStrongestAR,
    GeneratorContext &GC) {
  auto &MS = GC.getMemoryScheme();
  auto &SnpTgt = GC.getLLVMState().getSnippyTarget();
  auto ARRange = make_second_range(BaseRegToStrongestAR);
  std::vector<AddressRestriction> ARs(ARRange.begin(), ARRange.end());
  std::vector<AddressInfo> PrimaryAddresses =
      MS.randomBurstGroupAddresses(ARs, GC.getOpcodeCache(), SnpTgt);
  assert(PrimaryAddresses.size() == BaseRegToStrongestAR.size());
  std::map<unsigned, AddressInfo> BaseRegToPrimaryAddress;
  transform(
      zip(make_first_range(BaseRegToStrongestAR), std::move(PrimaryAddresses)),
      std::inserter(BaseRegToPrimaryAddress, BaseRegToPrimaryAddress.begin()),
      [](auto BaseRegToAI) {
        auto &&[BaseReg, AI] = BaseRegToAI;
        return std::make_pair(BaseReg, std::move(AI));
      });

  // Do additional randomization of immediate offsets for each address info to
  // have a uniform distribution imm offsets (otherwise, for the majority of
  // real memory schemes they'll be around zero).
  assert(BaseRegToStrongestAR.size() == BaseRegToPrimaryAddress.size());
  for (auto &&[RegToAR, RegToAI] :
       zip(BaseRegToStrongestAR, BaseRegToPrimaryAddress)) {
    auto &[Reg, AR] = RegToAR;
    auto &[BaseReg, AI] = RegToAI;
    assert(BaseReg == Reg);
    (void)BaseReg, (void)Reg;
    AI = randomlyShiftAddressOffsetsInImmRange(AI, AR.ImmOffsetRange);
  }
  return BaseRegToPrimaryAddress;
}

// Insert initialization of base addresses before the burst group.
static void
initializeBaseRegs(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                   std::map<unsigned, AddressInfo> &BaseRegToPrimaryAddress,
                   RegPoolWrapper &RP, GeneratorContext &GC) {
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  for (auto &[BaseReg, AI] : BaseRegToPrimaryAddress) {
    auto NewValue =
        APInt(SnippyTgt.getAddrRegLen(State.getTargetMachine()), AI.Address);
    assert(RP.isReserved(BaseReg, AccessMaskBit::W));
    if (GC.getGenSettings().TrackingConfig.AddressVH) {
      auto &I = GC.getOrCreateInterpreter();
      auto OldValue = I.readReg(BaseReg);
      SnippyTgt.transformValueInReg(MBB, Ins, OldValue, NewValue, BaseReg, RP,
                                    GC);
    } else
      SnippyTgt.writeValueToReg(MBB, Ins, NewValue, BaseReg, RP, GC);
  }
}

// This function returns address info to use for each opcode.
static std::vector<AddressInfo>
mapOpcodeIdxToAI(MachineBasicBlock &MBB, ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes, MachineBasicBlock::iterator Ins,
                 RegPoolWrapper &RP, GeneratorContext &SGCtx) {
  assert(OpcodeIdxToBaseReg.size() == Opcodes.size());
  if (Opcodes.empty())
    return {};

  // FIXME: This code does not account non-trivial cases when an opcode has
  // additional restrictions on the address register (e.g.
  // `excludeRegsForOpcode`).

  std::vector<AddressInfo> OpcodeIdxToAI;
  // Collect address restrictions for each opcode
  auto OpcodeToAR = collectAddressRestrictions(Opcodes, SGCtx, MBB);
  // For each base register we have a set of opcodes. Join address restrictions
  // for these set of opcodes by choosing the strongest ones and map the
  // resulting address restriction to the base register.
  auto BaseRegToStrongestAR =
      deduceStrongestRestrictions(Opcodes, OpcodeIdxToBaseReg, OpcodeToAR);
  // For the selected strongest restrictions get addresses. Thus, we'll have a
  // mapping from base register to a legal address in memory to use.
  auto BaseRegToPrimaryAddress =
      collectPrimaryAddresses(BaseRegToStrongestAR, SGCtx);
  // We've chosen addresses for each base register. Initialize base registers
  // with these addresses.
  initializeBaseRegs(MBB, Ins, BaseRegToPrimaryAddress, RP, SGCtx);

  // Try to find addresses for each opcode that allow better randomization of
  // offsets and effective addresses. If no address is found, we can always use
  // the primary one for the given base reg.
  for (auto [OpcodeIdx, Opcode] : enumerate(Opcodes)) {
    assert(OpcodeToAR.count(Opcode));
    assert(OpcodeIdxToBaseReg.size() > OpcodeIdx);
    const auto &OpcodeAR = OpcodeToAR[Opcode];
    auto BaseReg = OpcodeIdxToBaseReg[OpcodeIdx];
    assert(BaseRegToPrimaryAddress.count(BaseReg));
    const auto &OrigAI = BaseRegToPrimaryAddress[BaseReg];
    auto AI =
        selectAddressForSingleInstrFromBurstGroup(OrigAI, OpcodeAR, SGCtx);
    OpcodeIdxToAI.push_back(AI);
  }

  if (DumpMemAccesses.isSpecified())
    SGCtx.addBurstRangeMemAccess(OpcodeIdxToAI);

  return OpcodeIdxToAI;
}

static std::vector<unsigned>
generateBurstGroupOpcodes(IInstrGroupGenReq &BurstGroupRequest,
                          GeneratorContext &SGCtx) {
  auto Limit = BurstGroupRequest.getGenerationLimit(GenerationMode::NumInstrs);
  assert(Limit.has_value());

  std::vector<unsigned> ChosenOpcodes;
  std::generate_n(std::back_inserter(ChosenOpcodes), Limit.value(),
                  [&] { return BurstGroupRequest.genOpc(); });
  return ChosenOpcodes;
}

void InstructionGenerator::generateBurst(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    IInstrGroupGenReq &BurstGroupRequest) const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();

  if (GenerateInsertionPointHints)
    generateInsertionPointHint(MBB, MBB.end());

  auto Opcodes = generateBurstGroupOpcodes(BurstGroupRequest, *SGCtx);

  SmallVector<unsigned, 32> MemUsers;
  copy_if(Opcodes, std::back_inserter(MemUsers),
          [&SnippyTgt](auto Opc) -> bool {
            return SnippyTgt.countAddrsToGenerate(Opc);
          });
  auto RP = SGCtx->getRegisterPool();
  auto OpcodeIdxToBaseReg = generateBaseRegs(MBB, MemUsers, RP, *SGCtx);
  auto OpcodeIdxToAI =
      mapOpcodeIdxToAI(MBB, OpcodeIdxToBaseReg, MemUsers, Ins, RP, *SGCtx);
  unsigned MemUsersIdx = 0;
  for (auto Opcode : Opcodes) {
    const auto &InstrDesc = InstrInfo.get(Opcode);
    if (SnippyTgt.countAddrsToGenerate(Opcode)) {
      auto BaseReg = OpcodeIdxToBaseReg[MemUsersIdx];
      auto AI = OpcodeIdxToAI[MemUsersIdx];
      auto Preselected = selectOperands(InstrDesc, BaseReg, AI);
      const auto *MI =
          randomInstruction(InstrDesc, MBB, Ins, std::move(Preselected));

      assert(MI);
      auto [EffectiveAddr, AccessSize] =
          getEffectiveMemoryAccessInfo(MI, AI, SnippyTgt);
      if (DumpMemAccesses.isSpecified() ||
          DumpMemAccessesRestricted.isSpecified())
        SGCtx->addBurstPlainMemAccess(EffectiveAddr, AccessSize);

      ++MemUsersIdx;
    } else
      randomInstruction(InstrDesc, MBB, Ins);
  }
}

void InstructionGenerator::selfcheckOverflowGuard() const {
  const auto &SelfcheckSection = SGCtx->getSelfcheckSection();
  if (SCAddress >= SelfcheckSection.VMA + SelfcheckSection.Size)
    report_fatal_error("Selfcheck section overflow. Try to provide "
                       "\"selfcheck\" section description in layout file",
                       false);
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
  ST.storeValueToAddr(MBB, InsertPos, SCAddress, RegValue, RP, *SGCtx);
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
    const MachineInstrBuilder &MIB, RegPoolWrapper &RP,
    const MCOperandInfo &MCOpInfo, const PreselectedOpInfo &Preselected,
    unsigned OpIndex, ArrayRef<MachineOperand> PregeneratedOperands) const {
  auto OpType = MCOpInfo.OperandType;

  const auto &State = SGCtx->getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RegGen = SGCtx->getRegGen();

  const auto &MBB = *MIB->getParent();

  if (OpType == MCOI::OperandType::OPERAND_REGISTER) {
    assert(MCOpInfo.RegClass != -1);
    bool IsDst = Preselected.getFlags() & RegState::Define;
    Register Reg;
    if (Preselected.isTiedTo())
      Reg = PregeneratedOperands[Preselected.getTiedTo()].getReg();
    else if (Preselected.isReg())
      Reg = Preselected.getReg();
    else {
      auto RegClass = RegInfo.getRegClass(MCOpInfo.RegClass);
      const auto &InstDesc = MIB->getDesc();
      auto Exclude =
          SnippyTgt.excludeRegsForOperand(RegClass, *SGCtx, InstDesc, OpIndex);
      auto Include = SnippyTgt.includeRegs(RegClass);
      AccessMaskBit Mask = IsDst ? AccessMaskBit::W : AccessMaskBit::R;

      auto CustomMask =
          SnippyTgt.getCustomAccessMaskForOperand(*SGCtx, InstDesc, OpIndex);
      // TODO: add checks that target-specific and generic restrictions are
      // not conflicting.
      if (CustomMask != AccessMaskBit::None)
        Mask = CustomMask;
      auto RegOpt =
          RegGen.generate(RegClass, RegInfo, RP, MBB, Exclude, Include, Mask);
      if (!RegOpt)
        return std::nullopt;
      Reg = RegOpt.value();
    }
    SnippyTgt.reserveRegsIfNeeded(MIB->getOpcode(),
                                  /* isDst */ IsDst,
                                  /* isMem */ false, Reg, RP, *SGCtx,
                                  *MIB->getParent());
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
      auto Exclude = SnippyTgt.excludeFromMemRegsForOpcode(MIB->getOpcode());
      auto RegOpt = RegGen.generate(RegClass, RegInfo, RP, MBB, Exclude);
      if (!RegOpt)
        return std::nullopt;
      Reg = RegOpt.value();
    }
    // FIXME: RW mask is too restrictive for the majority of instructions.
    SnippyTgt.reserveRegsIfNeeded(
        MIB->getOpcode(),
        /* isDst */ Preselected.getFlags() & RegState::Define,
        /* isMem */ true, Reg, RP, *SGCtx, *MIB->getParent());
    return createRegAsOperand(Reg, Preselected.getFlags());
  }

  if (OpType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
    assert(Preselected.isUnset() || Preselected.isImm());
    StridedImmediate StridedImm;
    if (Preselected.isImm())
      StridedImm = Preselected.getImm();
    auto Op = SnippyTgt.generateTargetOperand(*SGCtx, MIB->getOpcode(), OpType,
                                              StridedImm);
    return Op;
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

static AddressGenInfo chooseAddrGenInfoForInstrCallback(
    LLVMContext &Ctx,
    std::optional<GeneratorContext::LoopGenerationInfo> CurLoopGenInfo,
    size_t AccessSize, size_t Alignment, const MemoryAccess &MemoryScheme) {
  (void)CurLoopGenInfo; // for future extensibility
  // AddressGenInfo for one element access.
  return AddressGenInfo::singleAccess(AccessSize, Alignment, false /* Burst */);
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

      if (DumpMemAccesses.isSpecified() ||
          DumpMemAccessesRestricted.isSpecified()) {
        for (auto Addr : ChosenAddresses)
          SGCtx->addMemAccess(Addr, AccessSize);
      }

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
    const MachineInstrBuilder &MIB, const MCInstrDesc &InstrDesc,
    RegPoolWrapper &RP,
    const std::vector<PreselectedOpInfo> &Preselected) const {
  SmallVector<MachineOperand, 8> PregeneratedOperands;
  assert(InstrDesc.getNumOperands() == Preselected.size());
  iota_range<unsigned long> PreIota(0, Preselected.size(),
                                    /* Inclusive */ false);
  for (const auto &[MCOpInfo, PreselOpInfo, Index] :
       zip(InstrDesc.operands(), Preselected, PreIota)) {
    auto OpOpt = pregenerateOneOperand(MIB, RP, MCOpInfo, PreselOpInfo, Index,
                                       PregeneratedOperands);
    if (!OpOpt)
      return std::nullopt;
    PregeneratedOperands.push_back(OpOpt.value());
  }
  return PregeneratedOperands;
}

SmallVector<MachineOperand, 8> InstructionGenerator::pregenerateOperands(
    const MachineInstrBuilder &MIB, const MachineBasicBlock &MBB,
    const MCInstrDesc &InstrDesc, RegPoolWrapper &RP,
    const std::vector<PreselectedOpInfo> &Preselected) const {
  auto &RegGenerator = SGCtx->getRegGen();
  auto &State = SGCtx->getLLVMState();
  auto &InstrInfo = State.getInstrInfo();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto IterNum = 0u;
  constexpr auto FailsMaxNum = 10000u;

  while (IterNum < FailsMaxNum) {
    RP.reset();
    SnippyTgt.excludeRegsForOpcode(InstrDesc.getOpcode(), RP, *SGCtx, MBB);
    RegGenerator.setRegContextForPlugin();
    auto PregeneratedOperandsOpt =
        tryToPregenerateOperands(MIB, InstrDesc, RP, Preselected);
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
    std::vector<PreselectedOpInfo> Preselected) const {
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
      pregenerateOperands(MIB, MBB, InstrDesc, RP, Preselected);
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

GenerationStatistics InstructionGenerator::generateInstrGroup(
    MachineBasicBlock &MBB, IInstrGroupGenReq &InstructionGroupRequest) {
  SNIPPY_DEBUG_BRIEF(__func__, InstructionGroupRequest, 4);
  GenerationStatistics CurrentInstructionGroupStats;
  while (!InstructionGroupRequest.isCompleted(CurrentInstructionGroupStats)) {
    auto Stats = processInstructionGeneration(MBB, InstructionGroupRequest,
                                              CurrentInstructionGroupStats);
    CurrentInstructionGroupStats.merge(Stats);

    SNIPPY_DEBUG_BRIEF(__func__, InstructionGroupRequest, 4);
  }
  return CurrentInstructionGroupStats;
}

GenerationStatistics
InstructionGenerator::generateForMBB(MachineBasicBlock &MBB,
                                     IMBBGenReq &BBGenRequest) {
  GenerationStatistics CurrentBBGenerationStats;

  for (auto &SubReq : BBGenRequest) {
    SNIPPY_DEBUG_BRIEF(__func__, SubReq, 2);
    auto InstructionGroupGenerationResult = generateInstrGroup(MBB, SubReq);
    SNIPPY_DEBUG_BRIEF("InstructionGroupGenerationResult",
                       InstructionGroupGenerationResult, 2);
    CurrentBBGenerationStats.merge(InstructionGroupGenerationResult);
    SNIPPY_DEBUG_BRIEF(__func__, SubReq, 2);
  }

  return CurrentBBGenerationStats;
}

MachineBasicBlock *InstructionGenerator::findNextBlock(
    const MachineBasicBlock *MBB,
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
InstructionGenerator::findNextBlockOnModel(const MachineBasicBlock &MBB) const {
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
    auto Success = I.step();
    assert(Success && "Branch execution must not fail");
    if (I.getPC() == PC)
      // pc + 0. Conditional branch is taken (Asm printer replaces BB's label
      // with zero because the actual offset is still unknown).
      return SnippyTgt.getBranchDestination(Branch);
  }
  llvm_unreachable(
      "Given BB doesn't have unconditional branch. All conditional branches "
      "were not taken. Successor is unknown.");
}

std::unique_ptr<IFunctionGenReq>
InstructionGenerator::createMFGenerationRequest(
    const MachineFunction &MF) const {
  const auto &GenPlan = getAnalysis<BlockGenPlanning>().get();
  const MCInstrDesc *FinalInstDesc = nullptr;
  auto LastInstrStr = SGCtx->getLastInstr();
  if (!LastInstrStr.empty()) {
    auto Opc = SGCtx->getOpcodeCache().code(LastInstrStr.str());
    if (Opc.has_value())
      FinalInstDesc = SGCtx->getOpcodeCache().desc(Opc.value());
  }
  switch (SGCtx->getGenerationMode()) {
  case GenerationMode::NumInstrs:
    return std::make_unique<FunctionGenReq<GenerationMode::NumInstrs>>(
        MF, GenPlan, FinalInstDesc, *SGCtx);
  case GenerationMode::Size:
    return std::make_unique<FunctionGenReq<GenerationMode::Size>>(
        MF, GenPlan, FinalInstDesc, *SGCtx);
  case GenerationMode::Mixed:
    return std::make_unique<FunctionGenReq<GenerationMode::Mixed>>(
        MF, GenPlan, FinalInstDesc, *SGCtx);
  }
  llvm_unreachable("Unknown generation mode!");
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

void InstructionGenerator::generateForFunction(
    MachineFunction &MF, IFunctionGenReq &FunctionGenRequest) {
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

    auto &BBReq = FunctionGenRequest.getMBBGenerationRequest(*MBB);

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
      assert(BBReq.isCompleted(GenerationStatistics{}) &&
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

      auto GeneratedStats = generateForMBB(*MBB, BBReq);
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
  while (!FunctionGenRequest.isCompleted(CurrMFGenStats)) {
    assert(MBB);
    assert(!NotVisited.empty() &&
           "There are no more block to insert instructions.");
    NotVisited.erase(MBB);

    auto &BBReq = FunctionGenRequest.getMBBGenerationRequest(*MBB);
    SNIPPY_DEBUG_BRIEF("request for dead BasicBlock", BBReq);

    auto GeneratedStats = generateForMBB(*MBB, BBReq);
    CurrMFGenStats.merge(std::move(GeneratedStats));
    SNIPPY_DEBUG_BRIEF("Function codegen", CurrMFGenStats);
    MBB = findNextBlock(nullptr, NotVisited);
  }

  finalizeFunction(MF, FunctionGenRequest, CurrMFGenStats);
}

void InstructionGenerator::finalizeFunction(
    MachineFunction &MF, IFunctionGenReq &Request,
    const GenerationStatistics &MFStats) {
  auto &State = SGCtx->getLLVMState();
  auto &MBB = MF.back();

  auto LastInstr = SGCtx->getLastInstr();
  bool EmptyLastInstr = LastInstr.empty();

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

  // Or to generate no instruction at all.
  if (EmptyLastInstr) {
    MBB.back().setPostInstrSymbol(MF, ExitSym);
    return;
  }

  for (auto &&FinalReq : Request.getFinalGenReqs(MFStats)) {
    assert(FinalReq && "FinalReq is empty!");
    generateInstrGroup(MF.back(), *FinalReq);
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
  assert(FunctionGenRequest);
  SNIPPY_DEBUG_BRIEF("request for function", *FunctionGenRequest);

  generateForFunction(MF, *FunctionGenRequest);
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
  if (DumpMemAccesses.isSpecified())
    dumpMemAccesses(DumpMemAccesses.getValue(), GenCtx.getMemAccesses(),
                    GenCtx.getBurstRangeAccesses(),
                    GenCtx.getBurstPlainAccesses(), /* Restricted */ false);

  if (DumpMemAccessesRestricted.isSpecified())
    dumpMemAccesses(DumpMemAccessesRestricted.getValue(),
                    GenCtx.getMemAccesses(), GenCtx.getBurstRangeAccesses(),
                    GenCtx.getBurstPlainAccesses(), /* Restricted */ true);

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
