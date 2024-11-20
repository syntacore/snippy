//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"

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

  void generateWriteValueSeq(APInt Value, MCRegister DestReg,
                             GeneratorContext &GC, RegPoolWrapper &RP,
                             const MachineBasicBlock &MBB,
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
  createTargetContext(const GeneratorContext &Ctx) const override {
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
  getRegsForSelfcheck(const MachineInstr &MI, const MachineBasicBlock &MBB,
                      const GeneratorContext &GenCtx) const override {
    reportUnimplementedError();
  }

  void generateRegsInit(MachineBasicBlock &MBB, const IRegisterState &R,
                        GeneratorContext &GC) const override {
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
  void instructionPostProcess(MachineInstr &MI, GeneratorContext &GC,
                              MachineBasicBlock::iterator Ins) const override {
    reportUnimplementedError();
  }

  virtual MachineInstr *generateFinalInst(MachineBasicBlock &MBB,
                                          GeneratorContext &GC,
                                          unsigned LastInstr) const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister> getRegsPreservedByABI() const override {
    reportUnimplementedError();
  }

  const MCRegisterClass &
  getRegClass(const GeneratorContext &Ctx, unsigned OperandRegClassID,
              unsigned OpIndex, unsigned Opcode, const MachineBasicBlock &MBB,
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

  void generateSpillToStack(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Ins, MCRegister Reg,
                            GeneratorContext &GC,
                            MCRegister SP) const override {
    reportUnimplementedError();
  }

  void generateReloadFromStack(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator Ins, MCRegister Reg,
                               GeneratorContext &GC,
                               MCRegister SP) const override {
    reportUnimplementedError();
  }

  void generatePopNoReload(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator Ins, MCRegister Reg,
                           GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  unsigned getRegBitWidth(MCRegister Reg, GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  MCRegister regIndexToMCReg(unsigned RegIdx, RegStorageType Storage,
                             GeneratorContext &GC) const override {
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

  unsigned getSpillSizeInBytes(MCRegister Reg,
                               GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                    LLVMState &State) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateCall(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins,
                             const Function &Target, GeneratorContext &GC,
                             bool AsSupport) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateCall(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins,
                             const Function &Target, GeneratorContext &GC,
                             bool AsSupport,
                             unsigned PreferredCallOpCode) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateTailCall(MachineBasicBlock &MBB, const Function &Target,
                                 const GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateReturn(MachineBasicBlock &MBB,
                               const LLVMState &State) const override {
    reportUnimplementedError();
  }

  MachineInstr *generateNop(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Ins,
                            const LLVMState &State) const override {
    reportUnimplementedError();
  }

  unsigned getTransformSequenceLength(APInt OldValue, APInt NewValue,
                                      MCRegister Register,
                                      GeneratorContext &GC) const override {
    reportUnimplementedError();
  }
  void transformValueInReg(MachineBasicBlock &MBB,
                           const MachineBasicBlock::iterator &, APInt OldValue,
                           APInt NewValue, MCRegister Register,
                           RegPoolWrapper &RP,
                           GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  void loadEffectiveAddressInReg(MachineBasicBlock &MBB,
                                 const MachineBasicBlock::iterator &,
                                 MCRegister Register, uint64_t BaseAddr,
                                 uint64_t Stride, MCRegister IndexReg,
                                 RegPoolWrapper &RP,
                                 GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  virtual MachineOperand
  generateTargetOperand(GeneratorContext &SGCtx, unsigned OpCode,
                        unsigned OpType,
                        const StridedImmediate &StridedImm) const override {
    reportUnimplementedError();
  }

  unsigned getMaxInstrSize() const override { reportUnimplementedError(); }

  std::set<unsigned>
  getPossibleInstrsSize(const GeneratorContext &GC) const override {
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

  std::vector<Register>
  getPhysRegsFromUnit(Register RegUnit,
                      const MCRegisterInfo &RI) const override {
    reportUnimplementedError();
  }

  std::vector<Register>
  getPhysRegsWithoutOverlaps(Register RegUnit,
                             const MCRegisterInfo &RI) const override {
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

  MachineBasicBlock *generateBranch(const MCInstrDesc &InstrDesc,
                                    MachineBasicBlock &MBB,
                                    GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                   GeneratorContext &GC) const override {
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

  bool isSelfcheckAllowed(unsigned Opcode) const override {
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
  getMCRegClassForBranch(const MachineInstr &Instr,
                         GeneratorContext &GC) const override {
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
                        const GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  LoopType getLoopType(MachineInstr &Branch) const override {
    reportUnimplementedError();
  }

  unsigned insertLoopInit(MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator Pos, MachineInstr &Branch,
                          ArrayRef<Register> ReservedRegs, unsigned NIter,
                          GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  LoopCounterInsertionResult
  insertLoopCounter(MachineBasicBlock::iterator Pos, MachineInstr &Branch,
                    ArrayRef<Register> ReservedRegs, unsigned NIter,
                    GeneratorContext &GC, RegToValueType &ExitingValues,
                    unsigned RegCounterOffset) const override {
    reportUnimplementedError();
  }

  virtual void initializeTargetPasses() const override {
    reportUnimplementedError();
  }

  unsigned countAddrsToGenerate(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  std::pair<AddressParts, MemAddresses>
  breakDownAddr(AddressInfo AddrInfo, const MachineInstr &MI, unsigned AddrIdx,
                GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  unsigned getWriteValueSequenceLength(APInt Value, MCRegister Register,
                                       GeneratorContext &GC) const override {
    reportUnimplementedError();
  }
  void writeValueToReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       APInt Value, unsigned DstReg, RegPoolWrapper &RP,
                       GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  void copyRegToReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                    MCRegister Rs, MCRegister Rd,
                    GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  void loadRegFromAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       uint64_t Addr, MCRegister Reg, RegPoolWrapper &RP,
                       GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  void storeRegToAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                      uint64_t Addr, MCRegister Reg, RegPoolWrapper &RP,
                      GeneratorContext &GC,
                      unsigned BytesToWrite) const override {
    reportUnimplementedError();
  }

  void storeValueToAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                        uint64_t Addr, APInt Value, RegPoolWrapper &RP,
                        GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  std::tuple<size_t, size_t>
  getAccessSizeAndAlignment(unsigned Opcode, GeneratorContext &GC,
                            const MachineBasicBlock &MBB) const override {
    reportUnimplementedError();
  }

  std::vector<Register>
  excludeFromMemRegsForOpcode(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  std::vector<Register> excludeRegsForOperand(const MCRegisterClass &RC,
                                              const GeneratorContext &GC,
                                              const MCInstrDesc &InstrDesc,
                                              unsigned Operand) const override {
    reportUnimplementedError();
  }

  std::vector<Register> includeRegs(const MCRegisterClass &RC) const override {
    reportUnimplementedError();
  }

  void reserveRegsIfNeeded(unsigned Opcode, bool isDst, bool isMem,
                           Register Reg, RegPoolWrapper &RP,
                           GeneratorContext &GC,
                           const MachineBasicBlock &MBB) const override {
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

  size_t getAccessSize(unsigned Opcode) const override {
    reportUnimplementedError();
  }

  bool isCall(unsigned Opcode) const override { reportUnimplementedError(); }

  std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const MachineBasicBlock &MBB,
                     const GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  bool groupMustHavePrimaryInstr(const MachineBasicBlock &MBB,
                                 const GeneratorContext &GC) const override {
    reportUnimplementedError();
  }
  std::function<bool(unsigned)>
  getDefaultPolicyFilter(const MachineBasicBlock &MBB,
                         const GeneratorContext &GC) const override {
    reportUnimplementedError();
  }

  std::vector<MCRegister> getGlobalStateRegs() const override {
    reportUnimplementedError();
  }

  bool canProduceNaN(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  bool isFloatingPoint(MCRegister Reg) const override {
    reportUnimplementedError();
  }

  bool isFloatingPoint(const MCInstrDesc &InstrDesc) const override {
    reportUnimplementedError();
  }

  std::unique_ptr<AsmPrinter>
  createAsmPrinter(LLVMTargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const override {
    reportUnimplementedError();
  }

  uint8_t getCodeAlignment(const GeneratorContext &GC) const override {
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
