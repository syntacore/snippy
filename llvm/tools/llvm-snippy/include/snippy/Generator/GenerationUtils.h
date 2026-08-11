//===-- GenerationUtils.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATIONUTILS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATIONUTILS_H

#include "snippy/Generator/GenerationLimit.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/MemAccessInfo.h"
#include "snippy/Generator/PreselectedOpInfo.h"
#include "snippy/GeneratorUtils/RegisterPool.h"

namespace llvm {
namespace snippy {
namespace planning {
struct InstructionRequest;
} // namespace planning
// For the given InstrDesc fill the vector of selected operands to account them
// in instruction generation procedure.
void selectMemoryOperands(
    const MCInstrDesc &InstrDesc, unsigned BaseReg, const AddressInfo &AI,
    SmallVectorImpl<planning::PreselectedOpInfo> &Preselected);

/// \brief Select all necessary operands for memory instructions.
///
/// Do not select source operands that don't have any restrictions on them. E.g.
/// stored register is not selected here as we don't care what register will be
/// used
///
/// \return map from selected registers to values they need to be initialized
/// with
std::map<unsigned, APInt> selectOperandsForMemoryInstructions(
    InstructionGenerationContext &InstrGenCtx, ArrayRef<unsigned> Opcodes,
    RegPoolWrapper &RP,
    std::vector<SmallVector<planning::PreselectedOpInfo, 8>>
        &OpcodeIdxToPreselectedOps);
/// \brief Select non-memory operands for instruction. Take into account
/// registers that are reserved as memory operands
void selectNonMemoryOperands(
    const MCInstrDesc &InstrDesc,
    SmallVectorImpl<planning::PreselectedOpInfo> &Preselected,
    planning::InstructionGenerationContext &InstrGenCtx, RegPoolWrapper &RP,
    DenseSet<Register> &Destinations, const DenseSet<Register> &Excluded = {});
void selectConcreteOffsets(
    InstructionGenerationContext &IGC, const MCInstrDesc &InstrDesc,
    SmallVectorImpl<planning::PreselectedOpInfo> &Preselected);

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

// \brief Selects memory and non-memory operands for the consecutive instruction
//  sequence.
// \return a std::map mapping memory registers to their initial values.
std::map<unsigned, APInt> selectOperandsForConsecutiveInstrs(
    InstructionGenerationContext &InstrGenCtx, const SnippyTarget &Tgt,
    RegPoolWrapper &RP, std::vector<planning::InstructionRequest> &BurstInstrs);

enum class MemAccessKind { BURST, REGULAR };
void markMemAccess(InstructionGenerationContext &IGC,
                   const MemAddresses &Addresses, size_t AccessSize,
                   const MCInstrDesc &InstrDesc);
void markMemAccessAsUsed(InstructionGenerationContext &IGC,
                         const MCInstrDesc &InstrDesc, const AddressInfo &AI,
                         MemAccessKind Kind, MemAccessInfo *MAI);

void addMemAccessToDump(const MemAddresses &ChosenAddresses, MemAccessInfo &MAI,
                        size_t AccessSize);
void dumpMemAccessesIfNeeded(const MemAccessInfo &MAI);

void initializeBaseRegs(
    InstructionGenerationContext &InstrGenCtx,
    const std::map<unsigned, APInt> &BaseRegToPrimaryAddress);

// This function returns address info to use for each opcode.
std::pair<std::map<unsigned, APInt>, std::vector<AddressInfo>>
mapOpcodeIdxToAI(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes);

MachineBasicBlock::iterator processGeneratedInstructions(
    MachineBasicBlock::iterator ItBegin,
    planning::InstructionGenerationContext &InstrGenCtx,
    const planning::RequestLimit &Limit);

MachineBasicBlock *createMachineBasicBlock(MachineFunction &MF);

std::string getMBBSectionName(const MachineBasicBlock &MBB);

GlobalVariable *getGVForMBB(const MachineBasicBlock &MBB, GlobalsPool &GP,
                            SnippyProgramContext &ProgCtx);

template <typename... DstArgs>
MachineInstrBuilder
getInstBuilder(MDNode *MetadataMark, const SnippyTarget &Tgt,
               MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
               LLVMContext &Context, const MCInstrDesc &Desc,
               DstArgs... DstReg) {
  static_assert(sizeof...(DstReg) <= 1, "Only 0 or 1 dst regs supported");
  MIMetadata MD({}, MetadataMark);
  auto MIB = BuildMI(MBB, Ins, MD, Desc, DstReg...);
  Tgt.addAsmPrinterFlags(*MIB.getInstr());
  return MIB;
}

template <typename... DstArgs>
MachineInstrBuilder
getSupportInstBuilder(const SnippyTarget &Tgt, MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator Ins, LLVMContext &Context,
                      const MCInstrDesc &Desc, DstArgs... DstReg) {
  return getInstBuilder(getMetadataMark(Context, SnippyMetadata::Support), Tgt,
                        MBB, Ins, Context, Desc, DstReg...);
}

template <typename... DstArgs>
MachineInstrBuilder
getFormAddrInstBuilder(const SnippyTarget &Tgt, MachineBasicBlock &MBB,
                       MachineBasicBlock::iterator Ins, LLVMContext &Context,
                       const MCInstrDesc &Desc, DstArgs... DstReg) {
  return getInstBuilder(getMetadataMark(Context,
                                        SnippyMetadata::FormAddrForCall,
                                        SnippyMetadata::Support),
                        Tgt, MBB, Ins, Context, Desc, DstReg...);
}

template <typename... DstArgs>
MachineInstrBuilder
getMainInstBuilder(const SnippyTarget &Tgt, MachineBasicBlock &MBB,
                   MachineBasicBlock::iterator Ins, LLVMContext &Context,
                   const MCInstrDesc &Desc, DstArgs... DstReg) {
  return getInstBuilder(/* MetadataMark */ nullptr, Tgt, MBB, Ins, Context,
                        Desc, DstReg...);
}

// Reorder the elements of Vec with the newly specified indices from Indices.
// The element with the number i becomes the Indices[i] position as a result.
// For example, reorder({190, -32, -8}, {1, 2, 0}) -> {-8, 190, -32}
template <typename ArrayT>
void reorder(ArrayT &Vec, MutableArrayRef<unsigned> Indices) {
  for (auto I = 0u; I < Vec.size(); ++I) {
    while (Indices[I] != I) {
      auto TgtIdx = Indices[I];
      std::swap(Vec[I], Vec[TgtIdx]);
      std::swap(Indices[I], Indices[TgtIdx]);
    }
  }
}

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATIONUTILS_H
