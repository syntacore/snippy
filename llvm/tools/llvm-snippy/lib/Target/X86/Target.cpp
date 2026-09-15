//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SnippyOperandGenerator.h"

#include "snippy/Config/Selfcheck.h"
#include "snippy/Simulator/Targets/X86.h"

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86AsmPrinter.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCStreamer.h"

#include <vector>

namespace llvm {
namespace snippy {

namespace {
class X86Config : public TargetConfigInterface {
  void mapConfig(yaml::IO &IO) override {}
  bool hasConfig() const override { return false; }
};

class X86GeneratorContext : public TargetGenContextInterface {};

class SnippyX86Target : public SnippyTarget {
public:
  SnippyX86Target() = default;

  void generateWriteValueSeq(InstructionGenerationContext &IGC, APInt Value,
                             MCRegister DestReg,
                             SmallVectorImpl<MCInst> &Insts) const override {
    reportUnimplementedError();
  }

  [[noreturn]] void reportUnimplementedError() const {
    snippy::fatal("sorry, X86 target is not implemented");
  }

  bool matchesArch(Triple::ArchType Arch) const override;

  std::unique_ptr<IRegisterState>
  createRegisterState(const TargetGenContextInterface &TgtGenCtx,
                      const TargetSubtargetInfo &ST) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<TargetGenContextInterface>
  createTargetContext(LLVMState &State, const Config &Cfg,
                      const TargetSubtargetInfo *STI,
                      const RegPoolWrapper &RP) const override {
    return std::make_unique<X86GeneratorContext>();
  }

  std::unique_ptr<TargetConfigInterface> createTargetConfig() const override {
    return std::make_unique<X86Config>();
  }

  void checkInstrTargetDependency(const OpcodeHistogram &H,
                                  const OpcodeCache &OpCC,
                                  const ProgramConfig &ProgramCfg,
                                  const PassConfig &PassCfg) const override {}

  bool isModeSwitchInstr(unsigned Opcode) const override { return false; }

  bool modeSwitchIsSupport(const SnippyProgramContext &ProgCtx) const override {
    return true;
  }

  bool needToGenerateModeSwitches(
      const SnippyProgramContext &ProgCtx) const override {
    return false;
  }

  double
  getModeSwitchProbability(const SnippyProgramContext &ProgCtx) const override {
    return 0.0;
  }

  void checkTrackingRestrictions(const OpcodeHistogram &H) const override {
    reportUnimplementedError();
  }

  Error checkOperandsReinitializationSupported(
      unsigned Opcode, const MCInstrInfo &InstrInfo) const override {
    reportUnimplementedError();
  }

  Error checkOperandsReinitializationForbidden(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  std::pair<std::shared_ptr<const ModeChangingContext>,
            std::function<bool(unsigned)>>
  selectModeChangeAndGetFilter(const SnippyProgramContext &ProgCtx,
                               const MachineBasicBlock &MBB,
                               MDNode *MetadataMark) const override {
    reportUnimplementedError();
  }

  void generateModeChange(const ModeChangingContext &MCC,
                          InstructionGenerationContext &IGC,
                          MDNode *MetadataMark) const override {
    reportUnimplementedError();
  }

  std::vector<Register>
  getRegsForSelfcheck(const MachineInstr &MI,
                      InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<SelfcheckTargetConfigInterface>
  createSelfcheckTargetConfig() const override {
    reportUnimplementedError();
  }

  std::string
  validateSelfcheckConfig(const SelfcheckConfig &SelfcheckCfg,
                          const OpcodeHistogram &Histogram) const override {
    reportUnimplementedError();
  }

  std::string getDefaultLastInstr() const override { return "INT3"; }

  void generateRegsInit(InstructionGenerationContext &IGC,
                        const IRegisterState &R) const override {
    reportUnimplementedError();
  }

  unsigned getFPRegsCount(const TargetSubtargetInfo &ST) const override {
    return 0;
  }

  bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const override {
    return false;
  }

  bool
  canBeGeneratedAsCommonInstr(const MCInstrDesc &InstrDesc) const override {
    return true;
  }

  void generateCustomInst(
      const MCInstrDesc &InstrDesc,
      planning::InstructionGenerationContext &InstrGenCtx,
      ArrayRef<planning::PreselectedOpInfo> Preselected) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<SnippyOperandGenerator>
  createOperandGenerator(planning::InstructionGenerationContext &IGC,
                         const MCInstrDesc &InstrDesc) const override {
    return nullptr;
  }

  bool needsLowering(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  void lowerInstruction(InstructionGenerationContext &IGC,
                        MachineInstr &MI) const override {
    reportUnimplementedError();
  }

  void instructionPostProcess(InstructionGenerationContext &IGC,
                              MachineInstr &MI) const override {

  }

  void
  generateCallToMemInitRoutine(InstructionGenerationContext &IGC,
                               size_t SectionStart, size_t SectionSize,
                               MemorySeedTy Seed,
                               const Function &ExternalGFunc) const override {
    reportUnimplementedError();
  }

  MemInitCallGenResult getSectionStateAfterMemInitRoutine(
      SnippyProgramContext &ProgCtx, const TargetSubtargetInfo &STI,
      size_t SectionSize, MemorySeedTy Seed) const override {
    reportUnimplementedError();
  }

  void
  generateRandomGenFunction(InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  unsigned getRandomGenFunctionMaxSize() const override {
    reportUnimplementedError();
  }

  std::vector<unsigned>
  getRegListForMemCpyForSMC(InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  void generateMemCpyForSMC(MachineFunction &MF,
                            SnippyProgramContext &ProgCtx) const override {
    reportUnimplementedError();
  }

  void generateMemorytInitializationAtAddresses(
      InstructionGenerationContext &IGC,
      const MemoryMap &Addresses) const override {
    reportUnimplementedError();
  }

  virtual MachineInstr *generateFinalInst(InstructionGenerationContext &IGC,
                                          unsigned LastInstr) const override {
    reportUnimplementedError();
  }

  std::vector<std::string> getCallerSavedRegGroups() const override {
    return {};
  }

  std::vector<std::string> getCallerSavedLiveRegGroups() const override {
    return {};
  }

  std::vector<MCRegister>
  getCallerSavedRegs(const MachineFunction &MF,
                     ArrayRef<std::string> RegGroups) const override {
    if (RegGroups.empty())
      return {};

    std::vector<MCRegister> CallerRegs;

    // x86_64 caller-saved (scratch) registers per System V ABI
    // These are volatile across function calls
    CallerRegs.insert(CallerRegs.end(),
                      {
                          // Argument registers (also scratch)
                          X86::RDI, X86::RSI, X86::RDX, X86::RCX, X86::R8,
                          X86::R9,

                          // Return value and scratch registers
                          X86::RAX,           // Return value
                          X86::R10, X86::R11, // Scratch/temporary

                          // TODO: add vector registers
                      });

    // Note: x86_64 has no dedicated frame pointer in the ABI,
    // but RBP and RBX are callee-saved (so NOT included)
    // RSP (stack pointer) is also callee-saved
    return CallerRegs;
  }

  std::vector<MCRegister>
  getCalleeSavedRegs(const MCSubtargetInfo &SubTgt) const override {
    reportUnimplementedError();
  }

  const MCRegisterClass &
  getRegClass(const InstructionGenerationContext &IGC,
              unsigned OperandRegClassID, unsigned OpIndex,
              const MCInstrDesc &InstrDesc,
              const MCRegisterInfo &RegInfo) const override {
    return RegInfo.getRegClass(OperandRegClassID);
  }

  std::vector<MCRegister> getRegsSuitableForSP() const override {
    return {X86::RSP};
  }

  std::vector<MCRegister>
  getRegsSuitableForRA(std::optional<unsigned> CallOpcode) const override {
    return {X86::R9};
  }

  void getImplicitDefRegs(unsigned Opcode,
                          SmallVectorImpl<MCRegister> &OutRegs) const override {
    return;
  }

  MCRegister getStackPointer() const override { return X86::RSP; }
  MCRegister getReturnAddress() const override { return X86::R9; }

  bool isRegClassSupported(MCRegister Reg) const override {
    reportUnimplementedError();
  }

  void generateSpillToStack(
      InstructionGenerationContext &IGC, MCRegister Reg, MCRegister SP,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    reportUnimplementedError();
  }

  void generateReloadFromStack(
      InstructionGenerationContext &IGC, MCRegister Reg, MCRegister SP,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    reportUnimplementedError();
  }

  void generatePopNoReload(InstructionGenerationContext &IGC,
                           MCRegister Reg) const override {
    reportUnimplementedError();
  }

  unsigned getRegBitWidth(MCRegister Reg,
                          InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  MCRegister regIndexToMCReg(InstructionGenerationContext &IGC, unsigned RegIdx,
                             RegStorageType Storage) const override {
    reportUnimplementedError();
  }

  RegStorageType regToStorage(Register Reg) const override {
    reportUnimplementedError();
  }

  unsigned regToIndex(Register Reg) const override {
    reportUnimplementedError();
  }

  unsigned getNumRegs(RegStorageType Storage,
                      const TargetSubtargetInfo &SubTgt) const override {
    reportUnimplementedError();
  }

  unsigned
  getSpillSizeInBytes(MCRegister Reg, SnippyProgramContext &ProgCtx,
                      const TargetSubtargetInfo &SubTgt) const override {
    reportUnimplementedError();
  }

  unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                    const LLVMState &State) const override {
    reportUnimplementedError();
  }

  MachineInstr *
  generateMemoryBarrier(InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target, MDNode *MetadataMark,
                             std::optional<unsigned> Opcode,
                             MCRegister RA) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateTailCall(InstructionGenerationContext &IGC,
                                 const Function &Target) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateReturn(InstructionGenerationContext &IGC,
                               MCRegister RA) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateNop(InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  unsigned getTransformSequenceLength(InstructionGenerationContext &IGC,
                                      APInt OldValue, APInt NewValue,
                                      MCRegister Register) const override {
    reportUnimplementedError();
  }
  void transformValueInReg(InstructionGenerationContext &IGC, APInt OldValue,
                           APInt NewValue, MCRegister Register) const override {
    reportUnimplementedError();
  }

  void loadEffectiveAddressInReg(InstructionGenerationContext &IGC,
                                 MCRegister Register, uint64_t BaseAddr,
                                 uint64_t Stride,
                                 MCRegister IndexReg) const override {
    reportUnimplementedError();
  }

  size_t getNumImmOperands(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  MachineOperand generateMemoryRelatedImmediate(
      const MCInstrDesc &InstrDesc, unsigned OperandIdx,
      const StridedImmediate &StridedImm, const SnippyProgramContext &ProgCtx,
      const CommonPolicyConfig &Cfg,
      ArrayRef<planning::PreselectedOpInfo> PregeneratedOperands,
      MemAddr Addr) const override {
    reportUnimplementedError();
  }

  MachineOperand
  generateTargetOperand(const MCInstrDesc &InstrDesc, unsigned OperandIdx,
                        const StridedImmediate &StridedImm,
                        const SnippyProgramContext &ProgCtx,
                        const CommonPolicyConfig &Cfg) const override {
    reportUnimplementedError();
  }

  unsigned getMaxInstrSize() const override { reportUnimplementedError(); }

  std::set<unsigned>
  getPossibleInstrsSize(const TargetSubtargetInfo &STI) const override {
    reportUnimplementedError();
  }

  bool isMultipleReg(Register Reg, const MCRegisterInfo &RI) const override {
    if (Reg == X86::NoRegister)
      return false;
    // If there is only one subreg in subregs,
    // then this register does not consist of smaller ones, which means it is
    // physical
    auto Subregs = RI.subregs_inclusive(Reg);
    return std::distance(Subregs.begin(), Subregs.end()) != 1;
  }

  bool isPhysRegClass(unsigned RegClassID,
                      const MCRegisterInfo &RI) const override {
    const auto &RC = RI.getRegClass(RegClassID);
    return std::all_of(RC.begin(), RC.end(), [this, &RI](unsigned Reg) {
      return !isMultipleReg(Reg, RI);
    });
  }

  Register getFirstPhysReg(Register Reg,
                           const MCRegisterInfo &RI) const override {
    auto Subregs = RI.subregs_inclusive(Reg);
    return *std::min_element(Subregs.begin(), Subregs.end());
  }

  void
  getSubregsInclusive(Register Reg, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    OutPhysRegs.clear();
    llvm::append_range(OutPhysRegs, RI.subregs_inclusive(Reg));
  }

  void
  getPhysRegsFromUnit(Register RegUnit, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    OutPhysRegs.clear();
    if (RegUnit == X86::NoRegister)
      return;
    if (!isMultipleReg(RegUnit, RI)) {
      OutPhysRegs.push_back(RegUnit);
      return;
    }

    auto Subregs = RI.subregs_inclusive(RegUnit);
    copy_if(Subregs, std::back_inserter(OutPhysRegs),
            [this, &RI](auto &SubReg) { return !isMultipleReg(SubReg, RI); });
  }

  void getPhysRegsWithoutOverlaps(
      Register RegUnit, const MCRegisterInfo &RI,
      SmallVectorImpl<Register> &OutPhysRegs) const override {
    reportUnimplementedError();
  }

  unsigned getMaxBranchDstMod(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock *
  getBranchDestination(const MachineInstr &Branch) const override {
    reportUnimplementedError();
  }

  bool mayBeScheduled(const MachineInstr &MI) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock *generateBranch(InstructionGenerationContext &IGC,
                                    const MCInstrDesc &InstrDesc,
                                    MachineBasicBlock *Dst) const override {
    reportUnimplementedError();
  }

  MachineInstr &insertIndirectJump(InstructionGenerationContext &IGC,
                                   MachineBasicBlock &TBB,
                                   unsigned Opcode) const override {
    reportUnimplementedError();
  }

  void insertFallbackBranch(MachineBasicBlock &From, MachineBasicBlock &To,
                            const LLVMState &State) const override {
    reportUnimplementedError();
  }

  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock &NewDestMBB) const override {
    reportUnimplementedError();
  }
  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock::iterator To) const override {
    reportUnimplementedError();
  }

  bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                         MachineBasicBlock &OldDestMBB,
                         MachineBasicBlock &NewDestMBB) const override {
    reportUnimplementedError();
  }

  void addTargetSpecificPasses(PassManagerWrapper &PM) const override {

  }

  void addTargetLegalizationPasses(PassManagerWrapper &PM) const override {

  }

  bool is64Bit(const TargetMachine &TM) const override {
    reportUnimplementedError();
  }

  bool isSelfcheckAllowed(const SnippyProgramContext &ProgCtx,
                          const SelfcheckConfig &SelfcheckCfg,
                          const MachineInstr &MI) const override {
    reportUnimplementedError();
  }

  bool isAtomicMemInstr(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  bool isVectorInstr(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  Error
  hasMandatoryTargetFeaturesForSMC(const MCSubtargetInfo &SI) const override {
    reportUnimplementedError();
  }

  bool isUnsupportedForSMC(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  void getEncodedMCInstr(const MachineInstr *MI, const MCCodeEmitter &MCCE,
                         AsmPrinter &AP, const MCSubtargetInfo &STI,
                         SmallVector<char> &OutBuf) const override {
    reportUnimplementedError();
  }

  SmallVector<unsigned>
  getImmutableRegs(const MCRegisterClass &MCRegClass) const override {
    reportUnimplementedError();
  }

  const MCRegisterClass &
  getMCRegClassForBranch(SnippyProgramContext &ProgCtx,
                         const MachineInstr &Instr) const override {
    reportUnimplementedError();
  }

  MachineInstr &
  updateLoopBranch(MachineInstr &Branch, const MCInstrDesc &InstrDesc,
                   ArrayRef<Register> ReservedRegs) const override {
    reportUnimplementedError();
  }

  unsigned
  getNumRegsForLoopBranch(const MCInstrDesc &BranchDesc) const override {
    reportUnimplementedError();
  }

  unsigned getLoopOverhead() const override { reportUnimplementedError(); }

  // FIXME: for other targets we use <TGT>InstrInfo::getInstSizeInBytes but it
  // is not implemented for X86 so we basically parse instruction type by-hand
  unsigned getInstrSize(const MachineInstr &Inst,
                        LLVMState &State) const override {
    const auto &X86STI =
        State.getSubtarget<X86Subtarget>(*Inst.getParent()->getParent());
    const auto *MCII = X86STI.getInstrInfo();
    const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());

    // 1. Fixed-size instructions
    unsigned FixedSize = Desc.getSize();
    if (FixedSize > 0)
      return FixedSize;

    // 2. For variable-length instructions, compute size from operands
    unsigned Size = 0;

    // Opcode bytes: 1 for most, 2 if 0x0F prefix (we infer from opcode enum)
    // We can check if the opcode belongs to the 0x0F group by looking at the
    // instruction's TSFlags, but to keep it simple, we'll assume 1 byte.
    Size += 1;

    // REX prefix (64-bit mode) – present if any 64-bit GPR is used
    bool HasREX = false;
    for (const auto &MO : Inst.operands()) {
      if (MO.isReg()) {
        unsigned Reg = MO.getReg();
        if (Reg >= X86::RAX && Reg <= X86::R15) {
          HasREX = true;
          break;
        }
      }
    }
    if (HasREX)
      Size += 1;

    // ModR/M byte – present for most instructions. We check if the instruction
    // has any memory operand or if the operands include registers that need it.
    // All GPR instructions (except some like PUSH/POP) have ModR/M.
    // To be safe, we always add it; only a few implicit instructions lack it.
    Size += 1;

    // SIB byte – used for complex addressing. We'll skip detection for now.

    // Displacement – check for memory operands via frame indices or
    // addressing modes. For simplicity, we'll add 4 bytes if we see a
    // frame index operand (common for stack references).
    for (const auto &MO : Inst.operands()) {
      if (MO.isFI()) {
        Size += 4; // typical displacement size
        break;
      }
    }

    // Immediate – check for immediate operands
    for (const auto &MO : Inst.operands()) {
      if (MO.isImm()) {
        int64_t Imm = MO.getImm();
        if (Imm >= std::numeric_limits<int8_t>::min() &&
            Imm <= std::numeric_limits<int8_t>::max())
          Size += 1; // sign-extended byte
        else
          Size += 4; // 32-bit immediate (or 8 for 64-bit, but rare)
      }
    }

    if (Size == 0)
      snippy::fatal(Twine("Internal error at: ") + __PRETTY_FUNCTION__,
                    "unknown instruction type");

    return Size;
  }

  LoopType getLoopType(MachineInstr &Branch) const override {
    reportUnimplementedError();
  }

  LoopCounterInitResult insertLoopInit(InstructionGenerationContext &IGC,
                                       MachineInstr &Branch,
                                       const Branchegram &Branches,
                                       ArrayRef<Register> ReservedRegs,
                                       unsigned NIter) const override {
    reportUnimplementedError();
  }

  LoopCounterInsertionResult insertLoopCounter(
      InstructionGenerationContext &IGC, MachineInstr &Branch,
      ArrayRef<Register> ReservedRegs, unsigned NIter,
      RegToValueType &ExitingValues,
      const LoopCounterInitResult &CounterInitInfo) const override {
    reportUnimplementedError();
  }

  virtual void initializeTargetPasses() const override {}

  unsigned countAddrsToGenerate(unsigned Opcode) const override { return 0; }

  std::pair<AddressParts, MemAddresses>
  breakDownAddr(InstructionGenerationContext &IGC, AddressInfo AddrInfo,
                const MCInstrDesc &InstrDesc,
                SmallVectorImpl<planning::PreselectedOpInfo> &Preselected,
                unsigned AddrIdx,
                std::optional<MemAddr> MainPart) const override {
    reportUnimplementedError();
  }

  unsigned getWriteValueSequenceLength(InstructionGenerationContext &IGC,
                                       APInt Value,
                                       MCRegister Register) const override {
    reportUnimplementedError();
  }
  void writeValueToReg(InstructionGenerationContext &IGC, APInt Value,
                       unsigned DstReg) const override {
    reportUnimplementedError();
  }
  void writeValueToCSR(InstructionGenerationContext &IGC, APInt Value,
                       unsigned DstReg) const override {
    reportUnimplementedError();
  }

  void copyRegToReg(InstructionGenerationContext &IGC, MCRegister Rs,
                    MCRegister Rd) const override {
    reportUnimplementedError();
  }

  MachineInstr *loadSymbolAddress(InstructionGenerationContext &IGC,
                                  unsigned DestReg,
                                  const GlobalValue *Target) const override {
    reportUnimplementedError();
  }

  void loadRegFromAddr(
      InstructionGenerationContext &IGC, uint64_t Addr, MCRegister Reg,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    reportUnimplementedError();
  }

  void loadRegFromAddrInReg(
      InstructionGenerationContext &IGC, MCRegister AddrReg, MCRegister Reg,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    reportUnimplementedError();
  }

  MCRegister
  getTmpRegisterForCheckSumSelfcheck(InstructionGenerationContext &IGC,
                                     const RegPoolWrapper &RP) const override {
    reportUnimplementedError();
  }

  MCRegister generateInitRegisterValueForCheckSumSelfcheck(
      InstructionGenerationContext &IGC, MachineBasicBlock::iterator Ins,
      const RegPoolWrapper &RP, MCRegister Reg) const override {
    reportUnimplementedError();
  }

  void generateRegMove(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       LLVMContext &Context, const MCInstrInfo &InstrInfo,
                       MCRegister SrcReg, MCRegister DstReg) const override {
    reportUnimplementedError();
  }
  void generateCheckSumForSelfcheck(
      InstructionGenerationContext &IGC, MCRegister DstReg, MCRegister SrcReg,
      std::optional<MCRegister> TmpReg) const override {
    reportUnimplementedError();
  }

  void generateCheckForCheckSumSelfcheck(InstructionGenerationContext &IGC,
                                         MCRegister AccReg,
                                         MCRegister RefReg) const override {
    reportUnimplementedError();
  }

  void storeRegToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                      MCRegister Reg, unsigned BytesToWrite) const override {
    reportUnimplementedError();
  }

  void storeValueToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                        APInt Value) const override {
    reportUnimplementedError();
  }

  void preselectAccessSizeOperand(InstructionGenerationContext &IGC,
                                  const MCInstrDesc &InstrDesc,
                                  SmallVectorImpl<planning::PreselectedOpInfo>
                                      &Preselected) const override {
    reportUnimplementedError();
  }

  AddressGenInfo selectAddrGenInfoForInstr(
      const SnippyProgramContext &ProgCtx, unsigned Opcode,
      const MachineBasicBlock &MBB,
      ArrayRef<planning::PreselectedOpInfo> Preselected = {}) const override {
    reportUnimplementedError();
  }

  void
  getSelectionOperandsOrder(InstructionGenerationContext &IGC,
                            const MCInstrDesc &InstrDesc,
                            SmallVectorImpl<unsigned> &Indices) const override {
    auto NumOperands = InstrDesc.getNumOperands();
    Indices.resize(NumOperands);
    std::iota(Indices.begin(), Indices.end(), 0u);
    return;
  }

  void excludeFromMemRegsForInstr(
      const MCInstrDesc &Instr, const MCRegisterInfo &RI,
      SmallVectorImpl<Register> &Regs,
      std::optional<MemAddr> Addr = std::nullopt,
      const CommonPolicyConfig *Cfg = nullptr) const override {
    reportUnimplementedError();
  }

  std::vector<Register> excludeRegsForOperand(
      InstructionGenerationContext &IGC, const MCRegisterClass &RC,
      const MCInstrDesc &InstrDesc, unsigned OpIndex,
      ArrayRef<planning::PreselectedOpInfo> PregeneratedOperands)
      const override {
    return {};
  }

  std::vector<Register> includeRegs(unsigned Opcode,
                                    const MCRegisterClass &RC) const override {
    return {};
  }

  const TargetRegisterClass &getAddrRegClass() const override {
    reportUnimplementedError();
  }

  unsigned getAddrRegLen(const TargetMachine &TM) const override {
    reportUnimplementedError();
  }

  bool canUseInBurstMode(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  bool canInitializeOperand(const MCInstrDesc &InstrDesc, unsigned OpIndex,
                            const LLVMState &State) const override {
    reportUnimplementedError();
  }

  bool shouldPreselectOperandInBurstMode(const MCInstrDesc &InstrDesc,
                                         unsigned OpIdx) const override {
    reportUnimplementedError();
  }

  StridedImmediate getImmOffsetRangeForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  unsigned getImmOffsetAlignmentForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  unsigned getInternalOpcode(unsigned Opc) const override { return Opc; }

  unsigned getOriginalOpcode(unsigned Opc) const override {
    reportUnimplementedError();
  }

  bool isCall(unsigned Opcode) const override { return false; }
  bool isSPRelative(unsigned Opcode) const override { return false; }

  void allocateMemoryInitializationRegs(
      InstructionGenerationContext &IGC,
      bool FollowCallingConvention) const override {
    reportUnimplementedError();
  }
  std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const SnippyProgramContext &ProgCtx,
                     const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister> getGlobalStateRegs() const override {
    reportUnimplementedError();
  }

  void
  appendTraceSNTFConverter(Observer *Obs, const LLVMState &State,
                           Interpreter &I, const PassConfig &PassCfg,
                           const SnippyProgramContext &ProgCtx) const override {
    reportUnimplementedError();
  }

  bool canProduceNaN(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  std::optional<std::pair<MCRegister, const MCRegisterClass *>>
  tryGetNaNRegisterAndClass(InstructionGenerationContext &InstrGenCtx,
                            MCRegister Reg) const override {
    reportUnimplementedError();
  }

  bool isFloatingPoint(MCRegister Reg) const override { return false; }

  bool isFloatingPoint(const MCInstrDesc &InstrDesc) const override {
    return false;
  }

  std::unique_ptr<AsmPrinter>
  createAsmPrinter(TargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const override {
    return std::make_unique<X86AsmPrinter>(TM, std::move(Streamer));
  }

  uint8_t getCodeAlignment(const TargetSubtargetInfo &STI) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock::iterator
  insertJumpThroughRelocation(InstructionGenerationContext &IGC,
                              uint64_t Addr) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock::iterator generateJump(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Ins,
                                           MachineBasicBlock &TBB,
                                           LLVMState &State) const override {
    reportUnimplementedError();
  }
  bool fitsCondBranch(uint64_t Distance) const override {
    reportUnimplementedError();
  }

  bool fitsUncondBranch(uint64_t Distance) const override {
    reportUnimplementedError();
  }

  void addAsmPrinterFlags(MachineInstr &MI) const override {}
}; // namespace

bool SnippyX86Target::matchesArch(Triple::ArchType Arch) const {
  return Arch == Triple::x86_64 || Arch == Triple::x86;
}

} // namespace

static SnippyTarget *getTheX86SnippyTarget() {
  static SnippyX86Target Target;
  return &Target;
}

void InitializeX86SnippyTarget() {
  SnippyTarget::registerTarget(getTheX86SnippyTarget());
}

} // namespace snippy
} // namespace llvm
