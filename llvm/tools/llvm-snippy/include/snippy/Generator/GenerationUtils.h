//===-- GenerationUtils.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GENERATION_UTILS_H
#define LLVM_TOOLS_SNIPPY_GENERATION_UTILS_H

#include "snippy/Generator/GeneratorContext.h"

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

std::vector<planning::PreselectedOpInfo> selectConcreteOffsets(
    const MCInstrDesc &InstrDesc,
    const std::vector<planning::PreselectedOpInfo> &Preselected,
    GeneratorContext &GC);

std::map<unsigned, AddressRestriction>
collectAddressRestrictions(ArrayRef<unsigned> Opcodes, GeneratorContext &GC,
                           const MachineBasicBlock &MBB);

std::map<unsigned, AddressRestriction> deduceStrongestRestrictions(
    ArrayRef<unsigned> Opcodes, ArrayRef<unsigned> OpcodeIdxToBaseReg,
    const std::map<unsigned, AddressRestriction> &OpcodeToAR);

AddressInfo randomlyShiftAddressOffsetsInImmRange(AddressInfo AI,
                                                  StridedImmediate ImmRange);

std::vector<unsigned> generateBaseRegs(MachineBasicBlock &MBB,
                                       ArrayRef<unsigned> Opcodes,
                                       RegPoolWrapper &RP,
                                       GeneratorContext &SGCtx);

AddressInfo
selectAddressForSingleInstrFromBurstGroup(AddressInfo OrigAI,
                                          const AddressRestriction &OpcodeAR,
                                          GeneratorContext &GC);

AddressGenInfo chooseAddrGenInfoForInstrCallback(
    LLVMContext &Ctx,
    std::optional<GeneratorContext::LoopGenerationInfo> CurLoopGenInfo,
    size_t AccessSize, size_t Alignment, const MemoryAccess &MemoryScheme);

enum class MemAccessKind { BURST, REGULAR };
void markMemAccessAsUsed(const MCInstrDesc &InstrDesc, const AddressInfo &AI,
                         MemAccessKind Kind, GeneratorContext &GC);

void addMemAccessToDump(const MemAddresses &ChosenAddresses,
                        GeneratorContext &GC, size_t AccessSize);
void dumpMemAccessesIfNeeded(GeneratorContext &GC);

void initializeBaseRegs(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    std::map<unsigned, AddressInfo> &BaseRegToPrimaryAddress,
    RegPoolWrapper &RP, GeneratorContext &GC);

// This function returns address info to use for each opcode.
std::vector<AddressInfo>
mapOpcodeIdxToAI(MachineBasicBlock &MBB, ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes, MachineBasicBlock::iterator Ins,
                 RegPoolWrapper &RP, GeneratorContext &SGCtx);

} // namespace snippy
} // namespace llvm
#endif
