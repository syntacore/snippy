//===-- RISCVUtils.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// RISCV depended utils
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RISCVUTILS_H
#define LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RISCVUTILS_H

#include "RISCVGenerated.h"
#include "RVVUnitConfig.h"

namespace llvm {
namespace snippy {

inline bool isLegalRVVInstr(unsigned Opcode, const RVVPrimaryConfig &Cfg,
                            unsigned VLEN, const RISCVSubtarget *ST) {
  if (!isRVV(Opcode))
    return false;

  auto [SEWVal, LMUL, VL] = Cfg;
  auto SEW = static_cast<unsigned>(SEWVal);
  auto ELEN = ST->getELen();

  if (!Cfg.isLegal(ELEN, VLEN)) {
    // From RISCV-V spec 1.0:
    //    vset{i}vl{i} and whole-register loads, stores, and moves do not depend
    //    upon vtype.
    // This means that they are legal even with an illegal configurations
    if (isRVVModeSwitch(Opcode) || isRVVWholeRegLoadStore(Opcode) ||
        isRVVWholeRegisterMove(Opcode))
      return true;
    return false;
  }
  // RVV loads and stores have an element width encoded in the opcode:
  // unit-stride and strided instructions encode element width, whereas indexed
  // encode index element width. It means that element width (EEW) may differ
  // from the SEW from VType. In this cases actual LMUL (called EMUL) does not
  // match the LMUL from VTYPE but it still must be bounds that are 1/8 <= EMUL
  // <= 8, EMUL = EEW / SEW * LMUL.
  if (isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
      isRVVStridedLoadStore(Opcode)) {
    // EEW is a data element width.
    auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
    return isValidEMUL(ELEN, SEW, EEW, LMUL);
  }
  if (isRVVIndexedLoadStore(Opcode)) {
    // EEW is an index element width.
    auto EEW = getIndexElementWidth(Opcode);
    return isValidEMUL(ELEN, SEW, EEW, LMUL);
  }
  // RVV segment loads/stores encodes not only EEW in the opcode, but number of
  // fields (NFIELDS) as well. This introduces one more restriction in addition
  // to the previous one on EMUL: EMUL * NFIELDS <= 8. Note: EMUL from the
  // previous inequality is a EMUL of data elements. So, for segment unit-stride
  // and strided instructions we should check:
  //   1/8 <= EMUL <= 8 and EMUL * NFIELDS <= 8.
  // But for segment indexed instructions we must check:
  //   1/8 <= EMUL <= 8 and LMUL * NFIELDS <= 8, because data multiplier is LMUL
  //   from the current VType.
  if (isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) {
    // EEW is a data element width.
    auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
    if (!isValidEMUL(ELEN, SEW, EEW, LMUL))
      return false;
    auto EMUL = computeEMUL(ELEN, SEW, EEW, LMUL);
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(EMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVIndexedSegLoadStore(Opcode)) {
    auto EEW = getIndexElementWidth(Opcode);
    if (!isValidEMUL(ELEN, SEW, EEW, LMUL))
      return false;
    // The EMUL * NFIELDS ≤ 8 constraint applies to the data vector register
    // group. The data vector register group has EEW=SEW, EMUL=LMUL
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVFloatingPoint(Opcode)) {
    assert(ST);
    if (SEW < 16u &&
        !(mayBeZvfh8BitIntConversion(Opcode) && ST->hasStdExtZvfh()))
      return false;
    if (SEW < 32u && !((isZvfh(Opcode) && ST->hasStdExtZvfh()) ||
                       (isZvfhmin(Opcode) && ST->hasStdExtZvfhmin()))) {
      // If the EEW of a vector floating-point operand does not correspond to a
      // supported IEEE floating-point type, the instruction encoding is
      // reserved.
      return false;
    }
  }
  if (isRVVExt(Opcode)) {
    // If the source EEW is not a supported width, or source EMUL would be below
    // the minimum legal LMUL, the instruction encoding is reserved.
    auto Factor = getRVVExtFactor(Opcode);
    assert(Factor <= SEW);
    assert(SEW % Factor == 0);
    auto EEW = SEW / Factor;
    if (!isLegalSEW(EEW))
      return false;
    return isValidEMUL(ELEN, SEW, EEW, LMUL);
  }
  // Zvfbfmin / Zvfbfwma are defined only for SEW = 16. A later widening check
  // would otherwise accept SEW = 8/32/64, because EEW = SEW * 2 is still a
  // legal element width.
  if (isZvfbf(Opcode) && SEW != 16u)
    return false;
  if (isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
      isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) {
    // Both widening and narrowing instructions use operands with EEW = SEW * 2.
    auto EEW = SEW * 2u;
    if (!isLegalSEW(EEW))
      return false;
    // Check that LMUL * 2 is also legal.
    return isValidEMUL(ELEN, SEW, EEW, LMUL);
  }
  if (isRVVGather16(Opcode)) {
    // The vrgatherei16.vv form uses SEW/LMUL for the data in vs2 but EEW=16 and
    // EMUL = (16/SEW)*LMUL for the indices in vs1.
    auto EEW = 16u;
    return isValidEMUL(ELEN, SEW, EEW, LMUL);
  }

  // Instructions from zvbc (carryless multiplication) extension
  // are defined only for SEW = 64.
  if (isZvbc(Opcode) && SEW != 64u)
    return false;
  return true;
}

template <typename SimulatorT>
MaxInstrBitsType readInstrCode(const SimulatorT &Sim, ProgramCounterType PC) {
  // We first read the first 2 bytes of the instruction (7 bits is enough for
  // determine the instruction size, but since there are no instructions smaller
  // than 2 bytes in RISCV, we can read them), recognize the size of the
  // instruction, and then, if it is not a compressed instruction, read 4 bytes.
  uint16_t BeginOfInstr;
  auto Err = Sim.readMem(
      PC, MutableArrayRef<char>(reinterpret_cast<char *>(&BeginOfInstr),
                                sizeof(BeginOfInstr)));
  SNIPPY_CHECK_ERROR(Err, "failed to readMem");
  auto InstructionSize = getInstructionSize(BeginOfInstr);
  if (InstructionSize == kCompressedInstrSize * RISCV_CHAR_BIT)
    return BeginOfInstr;
  if (InstructionSize != kMaxSupportedInstrSize * RISCV_CHAR_BIT)
    snippy::fatal("Currently only instructions up to 32 bits are supported.");
  uint32_t Instr;
  // Read the full 32-bit instruction.
  Err = Sim.readMem(PC, MutableArrayRef<char>(reinterpret_cast<char *>(&Instr),
                                              sizeof(Instr)));
  SNIPPY_CHECK_ERROR(Err, "failed to readMem");
  return Instr;
}

// Opcodes that can be generated in the RVV mode with the given config.
inline OpcodeFilterType createOpcodeFilter(const RVVPrimaryConfig &PrimaryCfg,
                                           unsigned VLEN,
                                           const RISCVSubtarget &ST
) {
  return [=, &ST](unsigned Opcode) {
    if (!isRVV(Opcode))
      return true;
    // Mode switches are always generated using the ModeChangingPolicy
    if (isRVVModeSwitch(Opcode))
      return false;
    return isLegalRVVInstr(Opcode, PrimaryCfg, VLEN, &ST);
  };
}

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_RISCV_RISCVUTILS_H
