//===-- GenerationUtils.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GENERATION_UTILS_H
#define LLVM_TOOLS_SNIPPY_GENERATION_UTILS_H

#include "snippy/Generator/GenerationLimit.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/LoopLatcherPass.h"

namespace llvm {
namespace snippy {
namespace planning {
class PreselectedOpInfo;
} // namespace planning

// For the given InstrDesc fill the vector of selected operands to account them
// in instruction generation procedure.
std::vector<planning::PreselectedOpInfo>
selectOperands(const MCInstrDesc &InstrDesc, unsigned BaseReg,
               const AddressInfo &AI);

std::vector<planning::PreselectedOpInfo>
selectInitializableOperandsRegisters(InstructionGenerationContext &IGC,
                                     const MCInstrDesc &InstrDesc);

std::vector<planning::PreselectedOpInfo>
getPreselectedForInstr(const MCInst &Inst);

std::vector<planning::PreselectedOpInfo> selectConcreteOffsets(
    InstructionGenerationContext &IGC, const MCInstrDesc &InstrDesc,
    const std::vector<planning::PreselectedOpInfo> &Preselected);

std::map<unsigned, AddressRestriction>
collectAddressRestrictions(ArrayRef<unsigned> Opcodes,
                           SnippyProgramContext &ProgCtx,
                           const MachineBasicBlock &MBB);

std::map<unsigned, AddressRestriction> deduceStrongestRestrictions(
    ArrayRef<unsigned> Opcodes, ArrayRef<unsigned> OpcodeIdxToBaseReg,
    const std::map<unsigned, AddressRestriction> &OpcodeToAR);

AddressInfo randomlyShiftAddressOffsetsInImmRange(AddressInfo AI,
                                                  StridedImmediate ImmRange);

std::vector<unsigned> generateBaseRegs(InstructionGenerationContext &IGC,
                                       ArrayRef<unsigned> Opcodes);

AddressInfo
selectAddressForSingleInstrFromBurstGroup(InstructionGenerationContext &IGC,
                                          AddressInfo OrigAI,
                                          const AddressRestriction &OpcodeAR);

AddressGenInfo chooseAddrGenInfoForInstrCallback(
    LLVMContext &Ctx,
    std::optional<SnippyLoopInfo::LoopGenerationInfo> CurLoopGenInfo,
    size_t AccessSize, size_t Alignment, const MemoryAccess &MemoryScheme);

enum class MemAccessKind { BURST, REGULAR };
void markMemAccessAsUsed(InstructionGenerationContext &IGC,
                         const MCInstrDesc &InstrDesc, const AddressInfo &AI,
                         MemAccessKind Kind, MemAccessInfo *MAI);

void addMemAccessToDump(const MemAddresses &ChosenAddresses, MemAccessInfo &MAI,
                        size_t AccessSize);
void dumpMemAccessesIfNeeded(const MemAccessInfo &MAI);

void initializeBaseRegs(
    InstructionGenerationContext &InstrGenCtx,
    std::map<unsigned, AddressInfo> &BaseRegToPrimaryAddress);

// This function returns address info to use for each opcode.
std::vector<AddressInfo>
mapOpcodeIdxToAI(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes);

MachineBasicBlock::iterator processGeneratedInstructions(
    MachineBasicBlock::iterator ItBegin,
    planning::InstructionGenerationContext &InstrGenCtx,
    const planning::RequestLimit &Limit);

MachineBasicBlock *createMachineBasicBlock(MachineFunction &MF);

std::string getMBBSectionName(const MachineBasicBlock &MBB);

template <typename... DstArgs>
MachineInstrBuilder
getInstBuilder(bool IsSupport, const SnippyTarget &Tgt, MachineBasicBlock &MBB,
               MachineBasicBlock::iterator Ins, LLVMContext &Context,
               const MCInstrDesc &Desc, DstArgs... DstReg) {
  static_assert(sizeof...(DstReg) <= 1, "Only 0 or 1 dst regs supported");
  MIMetadata MD({}, IsSupport ? getSupportMark(Context) : nullptr);
  auto MIB = BuildMI(MBB, Ins, MD, Desc, DstReg...);
  Tgt.addAsmPrinterFlags(*MIB.getInstr());
  return MIB;
}

template <typename... DstArgs>
MachineInstrBuilder
getSupportInstBuilder(const SnippyTarget &Tgt, MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator Ins, LLVMContext &Context,
                      const MCInstrDesc &Desc, DstArgs... DstReg) {
  return getInstBuilder(/* IsSupport */ true, Tgt, MBB, Ins, Context, Desc,
                        DstReg...);
}

template <typename... DstArgs>
MachineInstrBuilder
getMainInstBuilder(const SnippyTarget &Tgt, MachineBasicBlock &MBB,
                   MachineBasicBlock::iterator Ins, LLVMContext &Context,
                   const MCInstrDesc &Desc, DstArgs... DstReg) {
  return getInstBuilder(/* IsSupport */ false, Tgt, MBB, Ins, Context, Desc,
                        DstReg...);
}

} // namespace snippy
} // namespace llvm
#endif
