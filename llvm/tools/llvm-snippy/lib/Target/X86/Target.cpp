//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"

#include "snippy/Config/Selfcheck.h"
#include "snippy/Simulator/Targets/X86.h"

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"

#include <vector>

namespace llvm {
namespace snippy {

namespace {

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
  createRegisterState(const TargetSubtargetInfo &ST) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<TargetGenContextInterface>
  createTargetContext(LLVMState &State, const Config &Cfg,
                      const TargetSubtargetInfo *STI) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<TargetConfigInterface> createTargetConfig() const override {
    reportUnimplementedError();
  }

  void checkInstrTargetDependency(const OpcodeHistogram &H) const override {
    reportUnimplementedError();
  }

  bool needsGenerationPolicySwitch(unsigned Opcode) const override {
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
  validateSelfcheckConfig(const SelfcheckConfig &SelfcheckCfg) const override {
    reportUnimplementedError();
  }

  void generateRegsInit(InstructionGenerationContext &IGC,
                        const IRegisterState &R) const override {
    reportUnimplementedError();
  }

  unsigned getFPRegsCount(const TargetSubtargetInfo &ST) const override {
    reportUnimplementedError();
  }

  bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  void generateCustomInst(
      const MCInstrDesc &InstrDesc,
      planning::InstructionGenerationContext &InstrGenCtx) const override {
    reportUnimplementedError();
  }
  void instructionPostProcess(InstructionGenerationContext &IGC,
                              MachineInstr &MI) const override {
    reportUnimplementedError();
  }

  virtual MachineInstr *generateFinalInst(InstructionGenerationContext &IGC,
                                          unsigned LastInstr) const override {
    reportUnimplementedError();
  }

  std::vector<std::string> getCallerSavedRegGroups() const override {
    reportUnimplementedError();
  }

  std::vector<std::string> getCallerSavedLiveRegGroups() const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister>
  getCallerSavedRegs(const MachineFunction &MF,
                     ArrayRef<std::string> RegGroups) const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister>
  getRegsPreservedByABI(const MCSubtargetInfo &SubTgt) const override {
    reportUnimplementedError();
  }

  const MCRegisterClass &
  getRegClass(InstructionGenerationContext &IGC, unsigned OperandRegClassID,
              unsigned OpIndex, unsigned Opcode,
              const MCRegisterInfo &RegInfo) const override {
    reportUnimplementedError();
  }

  const MCRegisterClass &
  getRegClassSuitableForSP(const MCRegisterInfo &RI) const override {
    reportUnimplementedError();
  }

  std::function<bool(MCRegister)>
  filterSuitableRegsForStackPointer() const override {
    reportUnimplementedError();
  }

  MCRegister getStackPointer() const override { reportUnimplementedError(); }

  bool isRegClassSupported(MCRegister Reg) const override {
    reportUnimplementedError();
  }

  void generateSpillToStack(InstructionGenerationContext &IGC, MCRegister Reg,
                            MCRegister SP) const override {
    reportUnimplementedError();
  }

  void generateReloadFromStack(InstructionGenerationContext &IGC,
                               MCRegister Reg, MCRegister SP) const override {
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
  getSpillSizeInBytes(MCRegister Reg,
                      InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                    LLVMState &State) const override {
    reportUnimplementedError();
  }

  MachineInstr *
  generateMemoryBarrier(InstructionGenerationContext &IGC) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target,
                             bool AsSupport) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target, bool AsSupport,
                             unsigned PreferredCallOpCode) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateTailCall(InstructionGenerationContext &IGC,
                                 const Function &Target) const override {
    reportUnimplementedError();
  }

  MachineInstr *
  generateReturn(InstructionGenerationContext &IGC) const override {
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

  virtual MachineOperand
  generateTargetOperand(SnippyProgramContext &ProgCtx,
                        const CommonPolicyConfig &Cfg, unsigned OpCode,
                        unsigned OpType,
                        const StridedImmediate &StridedImm) const override {
    reportUnimplementedError();
  }

  unsigned getMaxInstrSize() const override { reportUnimplementedError(); }

  std::set<unsigned>
  getPossibleInstrsSize(const TargetSubtargetInfo &STI) const override {
    reportUnimplementedError();
  }

  bool isMultipleReg(Register Reg, const MCRegisterInfo &RI) const override {
    reportUnimplementedError();
  }

  bool isPhysRegClass(unsigned RegClassID,
                      const MCRegisterInfo &RI) const override {
    reportUnimplementedError();
  }

  Register getFirstPhysReg(Register Reg,
                           const MCRegisterInfo &RI) const override {
    reportUnimplementedError();
  }

  void
  getPhysRegsFromUnit(Register RegUnit, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    reportUnimplementedError();
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

  bool branchNeedsVerification(const MachineInstr &Branch) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock *
  generateBranch(InstructionGenerationContext &IGC,
                 const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                   SnippyProgramContext &ProgCtx) const override {
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

  bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                         MachineBasicBlock &OldDestMBB,
                         MachineBasicBlock &NewDestMBB) const override {
    reportUnimplementedError();
  }

  void addTargetSpecificPasses(PassManagerWrapper &PM) const override {
    reportUnimplementedError();
  }

  void addTargetLegalizationPasses(PassManagerWrapper &PM) const override {
    reportUnimplementedError();
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

  unsigned getInstrSize(const MachineInstr &Inst,
                        LLVMState &State) const override {
    reportUnimplementedError();
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

  virtual void initializeTargetPasses() const override {
    reportUnimplementedError();
  }

  unsigned countAddrsToGenerate(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  std::pair<AddressParts, MemAddresses>
  breakDownAddr(InstructionGenerationContext &IGC, AddressInfo AddrInfo,
                const MachineInstr &MI, unsigned AddrIdx) const override {
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

  void loadRegFromAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                       MCRegister Reg) const override {
    reportUnimplementedError();
  }

  void loadRegFromAddrInReg(InstructionGenerationContext &IGC,
                            MCRegister AddrReg, MCRegister Reg) const override {
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

  InstrMemAccessInfo
  getAccessSizeAndAlignment(SnippyProgramContext &ProgCtx, unsigned Opcode,
                            const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }

  void
  excludeFromMemRegsForOpcode(unsigned Opcode, const MCRegisterInfo &RI,
                              SmallVectorImpl<Register> &Regs) const override {
    reportUnimplementedError();
  }

  std::vector<Register> excludeRegsForOperand(InstructionGenerationContext &IGC,
                                              const MCRegisterClass &RC,
                                              const MCInstrDesc &InstrDesc,
                                              unsigned Operand) const override {
    reportUnimplementedError();
  }

  std::vector<Register> includeRegs(unsigned Opcode,
                                    const MCRegisterClass &RC) const override {
    reportUnimplementedError();
  }

  void reserveRegsIfNeeded(InstructionGenerationContext &IGC, unsigned Opcode,
                           bool isDst, bool isMem,
                           Register Reg) const override {
    reportUnimplementedError();
  }

  const TargetRegisterClass &getAddrRegClass() const override {
    reportUnimplementedError();
  }

  unsigned getAddrRegLen(const TargetMachine &TM) const override {
    reportUnimplementedError();
  }

  bool canUseInMemoryBurstMode(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  bool canInitializeOperand(const MCInstrDesc &InstrDesc,
                            unsigned OpIndex) const override {
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

  size_t getAccessSize(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  bool isCall(unsigned Opcode) const override { reportUnimplementedError(); }

  std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const SnippyProgramContext &ProgCtx,
                     const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }

  bool groupMustHavePrimaryInstr(const SnippyProgramContext &ProgCtx,
                                 const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }
  std::function<bool(unsigned)>
  getDefaultPolicyFilter(const SnippyProgramContext &ProgCtx,
                         const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister> getGlobalStateRegs() const override {
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

  bool isFloatingPoint(MCRegister Reg) const override {
    reportUnimplementedError();
  }

  bool isFloatingPoint(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<AsmPrinter>
  createAsmPrinter(TargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const override {
    reportUnimplementedError();
  }

  uint8_t getCodeAlignment(const TargetSubtargetInfo &STI) const override {
    reportUnimplementedError();
  }

  MachineBasicBlock::iterator generateJump(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Ins,
                                           MachineBasicBlock &TBB,
                                           LLVMState &State) const override {
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
