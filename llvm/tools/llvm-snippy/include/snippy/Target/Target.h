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

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/RegisterPool.h"
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
class GeneratorContext;

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

class SnippyTarget {
public:
  SnippyTarget() = default;

  virtual std::unique_ptr<TargetGenContextInterface>
  createTargetContext(const GeneratorContext &Ctx) const = 0;

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

  virtual MCRegister getStackPointer() const = 0;

  // NOTE: this list do not include stack pointer.
  virtual std::vector<MCRegister> getRegsPreservedByABI() const = 0;

  virtual void generateSpill(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins, MCRegister Reg,
                             GeneratorContext &GC) const = 0;

  virtual void generateReload(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator Ins, MCRegister Reg,
                              GeneratorContext &GC) const = 0;

  virtual void generatePopNoReload(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator Ins,
                                   MCRegister Reg,
                                   GeneratorContext &GC) const = 0;

  virtual unsigned getRegBitWidth(MCRegister Reg,
                                  GeneratorContext &GC) const = 0;

  virtual MCRegister regIndexToMCReg(unsigned RegIdx, RegStorageType Storage,
                                     GeneratorContext &GC) const = 0;

  virtual RegStorageType regToStorage(Register Reg) const = 0;

  virtual unsigned regToIndex(Register Reg) const = 0;

  virtual unsigned getNumRegs(RegStorageType Storage,
                              const TargetSubtargetInfo &SubTgt) const = 0;

  // Returns size of stack slot for Reg.
  virtual unsigned getSpillSizeInBytes(MCRegister Reg,
                                       GeneratorContext &GC) const = 0;

  // Returns minimum address alignment that can be used to generate register
  // spill.
  virtual unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                            GeneratorContext &GC) const = 0;

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
  getRegsForSelfcheck(const MachineInstr &MI, const MachineBasicBlock &MBB,
                      const GeneratorContext &GenCtx) const = 0;

  virtual GenPolicy
  getGenerationPolicy(const MachineBasicBlock &MBB, GeneratorContext &GenCtx,
                      std::optional<unsigned> BurstGroupID) const = 0;

  virtual std::unique_ptr<IRegisterState>
  createRegisterState(const TargetSubtargetInfo &ST) const = 0;

  virtual void generateRegsInit(MachineBasicBlock &MBB, const IRegisterState &R,
                                GeneratorContext &GC) const = 0;

  virtual void generateCustomInst(const MCInstrDesc &InstrDesc,
                                  MachineBasicBlock &MBB, GeneratorContext &GC,
                                  MachineBasicBlock::iterator Ins) const = 0;

  virtual bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const = 0;

  virtual void
  instructionPostProcess(MachineInstr &MI, GeneratorContext &GC,
                         MachineBasicBlock::iterator Ins) const = 0;

  // If BytesToWrite is zero, the whole register will be stored.
  virtual void storeRegToAddr(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator Ins, uint64_t Addr,
                              MCRegister Reg, RegPoolWrapper &RP,
                              GeneratorContext &GC,
                              unsigned BytesToWrite) const = 0;

  virtual void storeValueToAddr(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator Ins, uint64_t Addr,
                                APInt Value, RegPoolWrapper &RP,
                                GeneratorContext &GC) const = 0;

  virtual MachineOperand
  generateTargetOperand(GeneratorContext &SGCtx, unsigned OpCode,
                        unsigned OpType,
                        const StridedImmediate &StridedImm) const = 0;

  // Get target-specific access mask requirements for operand in instruction.
  // Returns AccessMaskBit::None if no such.
  virtual AccessMaskBit
  getCustomAccessMaskForOperand(GeneratorContext &SGCtx,
                                const MCInstrDesc &InstrDesc,
                                unsigned Operand) const {
    return AccessMaskBit::None;
  }

  virtual unsigned getTransformSequenceLength(APInt OldValue, APInt NewValue,
                                              MCRegister Register,
                                              GeneratorContext &GC) const = 0;

  virtual void transformValueInReg(MachineBasicBlock &MBB,
                                   const MachineBasicBlock::iterator &,
                                   APInt OldValue, APInt NewValue,
                                   MCRegister Register, RegPoolWrapper &RP,
                                   GeneratorContext &GC) const = 0;

  // Places value [ BaseAddr + Stride * IndexReg ] in Register
  virtual void loadEffectiveAddressInReg(MachineBasicBlock &MBB,
                                         const MachineBasicBlock::iterator &,
                                         MCRegister Register, uint64_t BaseAddr,
                                         uint64_t Stride, MCRegister IndexReg,
                                         RegPoolWrapper &RP,
                                         GeneratorContext &GC) const = 0;

  virtual unsigned getMaxInstrSize() const = 0;

  virtual unsigned getMaxBranchDstMod(unsigned Opcode) const = 0;

  virtual bool branchNeedsVerification(const MachineInstr &Branch) const = 0;

  virtual MachineBasicBlock *
  getBranchDestination(const MachineInstr &Branch) const = 0;

  virtual MachineBasicBlock *generateBranch(const MCInstrDesc &InstrDesc,
                                            MachineBasicBlock &MBB,
                                            GeneratorContext &GC) const = 0;

  virtual bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                           GeneratorContext &GC) const = 0;

  virtual void insertFallbackBranch(MachineBasicBlock &From,
                                    MachineBasicBlock &To,
                                    const LLVMState &State) const = 0;

  virtual bool replaceBranchDest(MachineInstr &Branch,
                                 MachineBasicBlock &NewDestMBB) const = 0;

  virtual bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                                 MachineBasicBlock &OldDestMBB,
                                 MachineBasicBlock &NewDestMBB) const = 0;

  virtual MachineInstr *generateFinalInst(MachineBasicBlock &MBB,
                                          GeneratorContext &GC,
                                          unsigned LastInstrOpc) const = 0;

  virtual MachineInstr *generateCall(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator Ins,
                                     const Function &Target,
                                     GeneratorContext &GC,
                                     bool AsSupport) const = 0;

  virtual MachineInstr *generateCall(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator Ins,
                                     const Function &Target,
                                     GeneratorContext &GC, bool AsSupport,
                                     unsigned PreferredCallOpCode) const = 0;

  virtual MachineInstr *generateTailCall(MachineBasicBlock &MBB,
                                         const Function &Target,
                                         const GeneratorContext &GC) const = 0;

  virtual MachineInstr *generateReturn(MachineBasicBlock &MBB,
                                       const LLVMState &State) const = 0;

  virtual MachineInstr *generateNop(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator Ins,
                                    const LLVMState &State) const = 0;

  virtual void addTargetSpecificPasses(PassManagerWrapper &PM) const = 0;
  virtual void addTargetLegalizationPasses(PassManagerWrapper &PM) const = 0;

  virtual void initializeTargetPasses() const = 0;

  virtual bool is64Bit(const TargetMachine &TM) const = 0;

  virtual bool isDivOpcode(unsigned Opcode) const { return false; }

  virtual bool isSelfcheckAllowed(unsigned Opcode) const = 0;

  virtual bool isAtomicMemInstr(const MCInstrDesc &InstrDesc) const = 0;

  virtual unsigned countAddrsToGenerate(unsigned Opcode) const = 0;

  virtual std::pair<AddressParts, MemAddresses>
  breakDownAddr(AddressInfo AddrInfo, const MachineInstr &MI, unsigned AddrIdx,
                GeneratorContext &GC) const = 0;

  virtual unsigned getWriteValueSequenceLength(APInt Value, MCRegister Register,
                                               GeneratorContext &GC) const = 0;

  virtual void writeValueToReg(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator Ins, APInt Value,
                               unsigned DstReg, RegPoolWrapper &RP,
                               GeneratorContext &GC) const = 0;

  virtual void loadRegFromAddr(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator Ins, uint64_t Addr,
                               MCRegister Reg, RegPoolWrapper &RP,
                               GeneratorContext &GC) const = 0;

  virtual std::tuple<size_t, size_t>
  getAccessSizeAndAlignment(unsigned Opcode, GeneratorContext &GC,
                            const MachineBasicBlock &MBB) const = 0;

  virtual std::vector<Register>
  excludeFromMemRegsForOpcode(unsigned Opcode) const = 0;

  // FIXME: basically, we should need only MCRegisterClass, but now
  // MCRegisterClass for RISCV does not fully express available regs.
  virtual std::vector<Register>
  excludeRegsForOperand(const MCRegisterClass &RC, const GeneratorContext &GC,
                        const MCInstrDesc &InstrDesc,
                        unsigned Operand) const = 0;

  virtual std::vector<Register>
  includeRegs(const MCRegisterClass &RC) const = 0;

  virtual void excludeRegsForOpcode(unsigned Opcode, RegPoolWrapper &RP,
                                    GeneratorContext &GC,
                                    const MachineBasicBlock &MBB) const = 0;

  virtual void reserveRegsIfNeeded(unsigned Opcode, bool isDst, bool isMem,
                                   Register Reg, RegPoolWrapper &RP,
                                   GeneratorContext &GC,
                                   const MachineBasicBlock &MBB) const = 0;

  virtual SmallVector<unsigned>
  getImmutableRegs(const MCRegisterClass &MCRegClass) const = 0;

  // Loop overhead in instructions
  // NOTE: may be in future in should return overhead size in bytes
  virtual unsigned getLoopOverhead() const = 0;

  virtual MachineInstr &updateLoopBranch(MachineInstr &Branch,
                                         const MCInstrDesc &InstrDesc,
                                         Register CounterReg,
                                         Register LimitReg) const = 0;

  virtual void insertLoopInit(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator Pos,
                              MachineInstr &Branch, Register LatchRegNum,
                              Register LimitRegNum, unsigned NIter,
                              GeneratorContext &GC) const = 0;

  struct LoopCounterInsertionResult {
    std::optional<SnippyDiagnosticInfo> Diag;
    unsigned NIter;
    APInt MinCounterVal;
  };

  virtual LoopCounterInsertionResult
  insertLoopCounter(MachineBasicBlock::iterator Pos, MachineInstr &MI,
                    Register LatchRegNum, Register LimitRegNum, unsigned NIter,
                    GeneratorContext &GC,
                    RegToValueType &ExitingValues) const = 0;

  virtual ~SnippyTarget();

  virtual void getEncodedMCInstr(const MachineInstr *MI,
                                 const MCCodeEmitter &MCCE, AsmPrinter &AP,
                                 const MCSubtargetInfo &STI,
                                 SmallVector<char> &OutBuf) const = 0;

  virtual const TargetRegisterClass &getAddrRegClass() const = 0;

  virtual unsigned getAddrRegLen(const TargetMachine &TM) const = 0;

  virtual bool canUseInMemoryBurstMode(unsigned Opcode) const = 0;

  virtual StridedImmediate
  getImmOffsetRangeForMemAccessInst(const MCInstrDesc &InstrDesc) const = 0;

  virtual size_t getAccessSize(unsigned Opcode) const = 0;

  virtual bool isCall(unsigned Opcode) const = 0;

private:
  virtual bool matchesArch(Triple::ArchType Arch) const = 0;

  const SnippyTarget *Next = nullptr;
};

inline const llvm::MachineOperand &getDividerOp(const MachineInstr &MI) {
  return MI.getOperand(2);
}

} // namespace snippy
} // namespace llvm
