//===-- Target.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Classes that handle the creation of target-specific objects. This is
/// similar to Target/TargetRegistry.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "TargetConfigIface.h"

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/AccessMaskBit.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/PassManagerWrapper.h"
#include "snippy/Simulator/Simulator.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/OpcodeGenerator.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace llvm {

class AsmPrinter;
class MCCodeEmitter;

namespace sys {
class DynamicLibrary;
} // namespace sys

namespace snippy {
struct SectionDesc;
class LLVMState;
class RegPoolWrapper;
class GeneratorSettings;
class SnippyProgramContext;
class StridedImmediate;

enum class LoopType;
struct TargetGenContextInterface {
  virtual ~TargetGenContextInterface() = 0;

  template <typename ImplT> ImplT &getImpl() {
    return static_cast<ImplT &>(*this);
  }

  template <typename ImplT> const ImplT &getImpl() const {
    return static_cast<const ImplT &>(*this);
  }
};

inline TargetGenContextInterface::~TargetGenContextInterface() {}

enum class RegStorageType { XReg, FReg, VReg };

struct AddressPart {
  // Operands to update.
  SmallVector<unsigned, 2> operands;
  // Value to write.
  APInt Value;
  // RegIster class to pick reg from.
  // Is null if random register pick is not allowed.
  const MCRegisterClass *RegClass;
  // Default bound register. Must use it if register pick fails
  // or is not allowed (RegClass is null)
  Register FixedReg;

  // Base constructor for AddressPart.
  AddressPart(Register Reg, APInt Value)
      : Value(Value), RegClass(nullptr), FixedReg(Reg) {}

  // Constructor for AddressPart from MachineOperand. It appends operands with
  // index of given operand.
  AddressPart(const MachineOperand &Operand, APInt Value)
      : AddressPart(Operand.getReg(), Value) {
    operands.emplace_back(Operand.getParent()->getOperandNo(&Operand));
  }

  // Constructor for AddressPart with MCRegisterClass initialization.
  AddressPart(const MachineOperand &Operand, APInt Value,
              const MCRegisterInfo &RI)
      : AddressPart(Operand, Value) {
    RegClass = &RI.getRegClass(
        Operand.getParent()->getDesc().operands()[operands.front()].RegClass);
  }
};

using RegToValueType = DenseMap<Register, APInt>;
using AddressParts = SmallVector<AddressPart>;
namespace planning {
class GenPolicy;
class InstructionGenerationContext;
} // namespace planning
using planning::InstructionGenerationContext;

class SnippyTarget {
public:
  SnippyTarget() = default;

  virtual void generateWriteValueSeq(InstructionGenerationContext &IGC,
                                     APInt Value, MCRegister DestReg,
                                     SmallVectorImpl<MCInst> &Insts) const = 0;

  virtual std::unique_ptr<TargetGenContextInterface>
  createTargetContext(LLVMState &State, const GeneratorSettings &GenSettings,
                      const TargetSubtargetInfo *STI) const = 0;

  virtual std::unique_ptr<TargetConfigInterface> createTargetConfig() const = 0;

  virtual std::unique_ptr<SimulatorInterface>
  createSimulator(llvm::snippy::DynamicLibrary &ModelLib,
                  const SimulationConfig &Cfg,
                  const TargetGenContextInterface *TgtGenCtx,
                  RVMCallbackHandler *CallbackHandler,
                  const TargetSubtargetInfo &Subtarget) const {
    errs() << "warning: the selected target does not provide a simulator\n";
    return {};
  }

  virtual const MCRegisterClass &
  getRegClass(InstructionGenerationContext &IGC, unsigned OperandRegClassID,
              unsigned OpIndex, unsigned Opcode,
              const MCRegisterInfo &RegInfo) const = 0;

  virtual const MCRegisterClass &
  getRegClassSuitableForSP(const MCRegisterInfo &RI) const = 0;

  virtual std::function<bool(MCRegister)>
  filterSuitableRegsForStackPointer() const = 0;

  virtual MCRegister getStackPointer() const = 0;

  // NOTE: this list do not include stack pointer.
  virtual std::vector<MCRegister> getRegsPreservedByABI() const = 0;

  void generateSpillToAddr(InstructionGenerationContext &IGC, MCRegister Reg,
                           MemAddr Addr) const;

  virtual void generateSpillToStack(InstructionGenerationContext &IGC,
                                    MCRegister Reg, MCRegister SP) const = 0;

  virtual void generateReloadFromStack(InstructionGenerationContext &IGC,
                                       MCRegister Reg, MCRegister SP) const = 0;

  void generateReloadFromAddr(InstructionGenerationContext &IGC, MCRegister Reg,
                              MemAddr Addr) const;

  virtual void generatePopNoReload(InstructionGenerationContext &IGC,
                                   MCRegister Reg) const = 0;

  virtual unsigned getRegBitWidth(MCRegister Reg,
                                  InstructionGenerationContext &IGC) const = 0;

  virtual MCRegister regIndexToMCReg(InstructionGenerationContext &IGC,
                                     unsigned RegIdx,
                                     RegStorageType Storage) const = 0;

  virtual RegStorageType regToStorage(Register Reg) const = 0;

  virtual unsigned regToIndex(Register Reg) const = 0;

  virtual unsigned getNumRegs(RegStorageType Storage,
                              const TargetSubtargetInfo &SubTgt) const = 0;

  // Returns size of stack slot for Reg.
  virtual unsigned
  getSpillSizeInBytes(MCRegister Reg,
                      InstructionGenerationContext &IGC) const = 0;

  // Returns minimum address alignment that can be used to generate register
  // spill.
  virtual unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                            LLVMState &State) const = 0;

  virtual uint8_t getCodeAlignment(const TargetSubtargetInfo &IGC) const = 0;

  // Find register by name, std::nullopt if not found.
  virtual std::optional<unsigned>
  findRegisterByName(const StringRef RegName) const {
    return std::nullopt;
  }

  // default behavior is enabled
  virtual bool checkOpcodeSupported(int Opcode,
                                    const MCSubtargetInfo &SI) const {
    return true;
  }

  // pseudo instructions are disallowed by default
  virtual bool isPseudoAllowed(unsigned Opcode) const { return false; }

  // Returns the SnippyTarget for the given triple or nullptr if the target
  // does not exist.
  static const SnippyTarget *lookup(Triple TT);

  // Returns the default (unspecialized) SnippyTarget.
  static const SnippyTarget &getDefault();

  // Registers a target. Not thread safe.
  static void registerTarget(SnippyTarget *T);

  // Returns reserved memory region which Desc interfere with if any. Returns
  // nullptr if none such regions exists.
  virtual SectionDesc const *
  touchesReservedRegion(SectionDesc const &Desc) const {
    return nullptr;
  }

  virtual void checkInstrTargetDependency(const OpcodeHistogram &H) const = 0;

  virtual bool needsGenerationPolicySwitch(unsigned Opcode) const = 0;

  virtual std::vector<Register>
  getRegsForSelfcheck(const MachineInstr &MI,
                      InstructionGenerationContext &IGC) const = 0;

  virtual std::unique_ptr<IRegisterState>
  createRegisterState(const TargetSubtargetInfo &ST) const = 0;

  virtual void generateRegsInit(InstructionGenerationContext &IGC,
                                const IRegisterState &R) const = 0;

  virtual void generateCustomInst(
      const MCInstrDesc &InstrDesc,
      planning::InstructionGenerationContext &InstrGenCtx) const = 0;

  virtual bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const = 0;

  virtual void instructionPostProcess(InstructionGenerationContext &IGC,
                                      MachineInstr &MI) const = 0;

  // If BytesToWrite is zero, the whole register will be stored.
  virtual void storeRegToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                              MCRegister Reg, unsigned BytesToWrite) const = 0;

  virtual void storeValueToAddr(InstructionGenerationContext &IGC,
                                uint64_t Addr, APInt Value) const = 0;

  virtual bool isFloatingPoint(MCRegister Reg) const = 0;

  virtual bool isFloatingPoint(const MCInstrDesc &InstrDesc) const = 0;

  virtual MachineOperand
  generateTargetOperand(SnippyProgramContext &ProgCtx,
                        const GeneratorSettings &GenSettings, unsigned OpCode,
                        unsigned OpType,
                        const StridedImmediate &StridedImm) const = 0;

  virtual bool canProduceNaN(const MCInstrDesc &InstrDesc) const = 0;

  // Get target-specific access mask requirements for operand in instruction.
  // Returns AccessMaskBit::None if no such.
  virtual AccessMaskBit
  getCustomAccessMaskForOperand(const MCInstrDesc &InstrDesc,
                                unsigned Operand) const {
    return AccessMaskBit::None;
  }

  virtual unsigned getTransformSequenceLength(InstructionGenerationContext &IGC,
                                              APInt OldValue, APInt NewValue,
                                              MCRegister Register) const = 0;

  virtual void transformValueInReg(InstructionGenerationContext &IGC,
                                   APInt OldValue, APInt NewValue,
                                   MCRegister Register) const = 0;

  // Places value [ BaseAddr + Stride * IndexReg ] in Register
  virtual void loadEffectiveAddressInReg(InstructionGenerationContext &IGC,
                                         MCRegister Register, uint64_t BaseAddr,
                                         uint64_t Stride,
                                         MCRegister IndexReg) const = 0;

  virtual unsigned getMaxInstrSize() const = 0;

  virtual std::set<unsigned>
  getPossibleInstrsSize(const TargetSubtargetInfo &STI) const = 0;

  virtual bool isMultipleReg(Register Reg, const MCRegisterInfo &RI) const = 0;

  virtual bool isPhysRegClass(unsigned RegClassID,
                              const MCRegisterInfo &RI) const = 0;

  virtual Register getFirstPhysReg(Register Reg,
                                   const MCRegisterInfo &RI) const = 0;

  virtual std::vector<Register>
  getPhysRegsFromUnit(Register RegUnit, const MCRegisterInfo &RI) const = 0;

  // This function is different from getPhysRegsFromUnit in that
  // it returns physical registers without overlaps.
  // 1. If RegUnit is already a physical register, we will return it.
  // 2. If RegUnit is part of a physical register, we will return that part.
  // 3. Otherwise, we will return the physical register referenced by this
  // alias.
  virtual std::vector<Register>
  getPhysRegsWithoutOverlaps(Register RegUnit,
                             const MCRegisterInfo &RI) const = 0;

  virtual unsigned getMaxBranchDstMod(unsigned Opcode) const = 0;

  virtual bool branchNeedsVerification(const MachineInstr &Branch) const = 0;

  virtual MachineBasicBlock *
  getBranchDestination(const MachineInstr &Branch) const = 0;

  virtual MachineBasicBlock *
  generateBranch(InstructionGenerationContext &IGC,
                 const MCInstrDesc &InstrDesc) const = 0;

  virtual bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                           SnippyProgramContext &ProgCtx) const = 0;

  virtual void insertFallbackBranch(MachineBasicBlock &From,
                                    MachineBasicBlock &To,
                                    const LLVMState &State) const = 0;

  virtual bool replaceBranchDest(MachineInstr &Branch,
                                 MachineBasicBlock &NewDestMBB) const = 0;

  virtual bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                                 MachineBasicBlock &OldDestMBB,
                                 MachineBasicBlock &NewDestMBB) const = 0;

  virtual MachineInstr *generateFinalInst(InstructionGenerationContext &IGC,
                                          unsigned LastInstrOpc) const = 0;

  virtual MachineInstr *generateCall(InstructionGenerationContext &IGC,
                                     const Function &Target,
                                     bool AsSupport) const = 0;

  virtual MachineInstr *generateCall(InstructionGenerationContext &IGC,
                                     const Function &Target, bool AsSupport,
                                     unsigned PreferredCallOpCode) const = 0;

  virtual MachineInstr *generateTailCall(InstructionGenerationContext &IGC,
                                         const Function &Target) const = 0;

  virtual MachineInstr *
  generateReturn(InstructionGenerationContext &IGC) const = 0;

  virtual MachineInstr *
  generateNop(InstructionGenerationContext &IGC) const = 0;

  virtual void addTargetSpecificPasses(PassManagerWrapper &PM) const = 0;
  virtual void addTargetLegalizationPasses(PassManagerWrapper &PM) const = 0;

  virtual void initializeTargetPasses() const = 0;

  virtual bool is64Bit(const TargetMachine &TM) const = 0;

  virtual bool isDivOpcode(unsigned Opcode) const { return false; }

  virtual bool isSelfcheckAllowed(unsigned Opcode) const = 0;

  virtual bool isAtomicMemInstr(const MCInstrDesc &InstrDesc) const = 0;

  virtual unsigned countAddrsToGenerate(unsigned Opcode) const = 0;

  virtual std::pair<AddressParts, MemAddresses>
  breakDownAddr(InstructionGenerationContext &IGC, AddressInfo AddrInfo,
                const MachineInstr &MI, unsigned AddrIdx) const = 0;

  virtual unsigned
  getWriteValueSequenceLength(InstructionGenerationContext &IGC, APInt Value,
                              MCRegister Register) const = 0;

  virtual void writeValueToReg(InstructionGenerationContext &IGC, APInt Value,
                               unsigned DstReg) const = 0;

  virtual void copyRegToReg(InstructionGenerationContext &IGC, MCRegister Rs,
                            MCRegister Rd) const = 0;

  virtual void loadRegFromAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                               MCRegister Reg) const = 0;

  virtual std::tuple<size_t, size_t>
  getAccessSizeAndAlignment(SnippyProgramContext &ProgCtx, unsigned Opcode,
                            const MachineBasicBlock &MBB) const = 0;

  virtual std::vector<Register>
  excludeFromMemRegsForOpcode(unsigned Opcode) const = 0;

  // FIXME: basically, we should need only MCRegisterClass, but now
  // MCRegisterClass for RISCV does not fully express available regs.
  virtual std::vector<Register>
  excludeRegsForOperand(InstructionGenerationContext &IGC,
                        const MCRegisterClass &RC, const MCInstrDesc &InstrDesc,
                        unsigned Operand) const = 0;

  virtual std::vector<Register>
  includeRegs(const MCRegisterClass &RC) const = 0;

  virtual void reserveRegsIfNeeded(InstructionGenerationContext &IGC,
                                   unsigned Opcode, bool isDst, bool isMem,
                                   Register Reg) const = 0;

  virtual SmallVector<unsigned>
  getImmutableRegs(const MCRegisterClass &MCRegClass) const = 0;

  // Loop overhead in instructions
  // NOTE: may be in future in should return overhead size in bytes
  virtual unsigned getLoopOverhead() const = 0;

  virtual const MCRegisterClass &
  getMCRegClassForBranch(SnippyProgramContext &ProgCtx,
                         const MachineInstr &Instr) const = 0;

  virtual MachineInstr &
  updateLoopBranch(MachineInstr &Branch, const MCInstrDesc &InstrDesc,
                   ArrayRef<Register> ReservedRegs) const = 0;

  virtual unsigned
  getNumRegsForLoopBranch(const MCInstrDesc &BranchDesc) const = 0;

  virtual unsigned getInstrSize(const MachineInstr &Inst,
                                LLVMState &State) const = 0;

  // returns min loop counter value
  virtual unsigned insertLoopInit(InstructionGenerationContext &IGC,
                                  MachineInstr &Branch,
                                  ArrayRef<Register> ReservedRegs,
                                  unsigned NIter) const = 0;

  virtual LoopType getLoopType(MachineInstr &Branch) const = 0;

  struct LoopCounterInsertionResult {
    std::optional<SnippyDiagnosticInfo> Diag;
    unsigned NIter;
    APInt MinCounterVal;
  };

  virtual LoopCounterInsertionResult
  insertLoopCounter(InstructionGenerationContext &IGC, MachineInstr &MI,
                    ArrayRef<Register> ReservedRegs, unsigned NIter,
                    RegToValueType &ExitingValues,
                    unsigned RegCounterOffset) const = 0;

  virtual ~SnippyTarget();

  virtual void getEncodedMCInstr(const MachineInstr *MI,
                                 const MCCodeEmitter &MCCE, AsmPrinter &AP,
                                 const MCSubtargetInfo &STI,
                                 SmallVector<char> &OutBuf) const = 0;

  virtual const TargetRegisterClass &getAddrRegClass() const = 0;

  virtual unsigned getAddrRegLen(const TargetMachine &TM) const = 0;

  virtual bool canUseInMemoryBurstMode(unsigned Opcode) const = 0;

  virtual bool canInitializeOperand(const MCInstrDesc &InstrDesc,
                                    unsigned OpIndex) const = 0;

  virtual StridedImmediate
  getImmOffsetRangeForMemAccessInst(const MCInstrDesc &InstrDesc) const = 0;

  virtual size_t getAccessSize(unsigned Opcode) const = 0;

  virtual bool isCall(unsigned Opcode) const = 0;

  virtual std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const SnippyProgramContext &ProgCtx,
                     const MachineBasicBlock &MBB) const = 0;

  // Currently the result is the same for all groups in BB
  virtual bool
  groupMustHavePrimaryInstr(const SnippyProgramContext &ProgCtx,
                            const MachineBasicBlock &MBB) const = 0;
  virtual std::function<bool(unsigned)>
  getDefaultPolicyFilter(const SnippyProgramContext &ProgCtx,
                         const MachineBasicBlock &MBB) const = 0;
  // Registers that should be valid while calling an external function
  virtual std::vector<MCRegister> getGlobalStateRegs() const = 0;

  virtual std::unique_ptr<AsmPrinter>
  createAsmPrinter(LLVMTargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const = 0;

  virtual MachineBasicBlock::iterator
  generateJump(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
               MachineBasicBlock &TBB, LLVMState &State) const = 0;

  // Add any additional target-dependent flags to provide additional information
  // to asm printer.
  virtual void addAsmPrinterFlags(MachineInstr &MI) const = 0;

private:
  virtual bool matchesArch(Triple::ArchType Arch) const = 0;

  const SnippyTarget *Next = nullptr;
};

inline const llvm::MachineOperand &getDividerOp(const MachineInstr &MI) {
  return MI.getOperand(2);
}

} // namespace snippy
} // namespace llvm
