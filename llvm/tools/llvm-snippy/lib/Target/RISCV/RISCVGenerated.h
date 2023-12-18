//===-- RISCVGenerated.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This stuff will end up being generated
///
//===----------------------------------------------------------------------===//

#pragma once

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVRegisterInfo.h"

namespace llvm {
namespace snippy {

#ifdef ATOMIC_INST_PLACER
#error "defined internal-only define ATOMIC_INST_PLACER"
#endif
#define ATOMIC_INST_PLACER(BASIC_OPCODE)                                       \
  BASIC_OPCODE:                                                                \
  case BASIC_OPCODE##_AQ:                                                      \
  case BASIC_OPCODE##_AQ_RL:                                                   \
  case BASIC_OPCODE##_RL

#ifdef RVV_SEG_EACHFIELD_PLACER
#error "defined internal-only define RVV_SEG_EACHFIELD_PLACER"
#endif
// clang-format off
#define RVV_SEG_EACHFIELD_PLACER(BASIC_OPCODE, EEW)                            \
  BASIC_OPCODE##2E##EEW##_V:                                                   \
  case BASIC_OPCODE##3E##EEW##_V:                                              \
  case BASIC_OPCODE##4E##EEW##_V:                                              \
  case BASIC_OPCODE##5E##EEW##_V:                                              \
  case BASIC_OPCODE##6E##EEW##_V:                                              \
  case BASIC_OPCODE##7E##EEW##_V:                                              \
  case BASIC_OPCODE##8E##EEW##_V
// clang-format on

// clang-format off
#define RV_FLOATING_POINT(PRECISION)                                           \
    case RISCV::FMADD_##PRECISION: \
    case RISCV::FMSUB_##PRECISION: \
    case RISCV::FNMSUB_##PRECISION: \
    case RISCV::FNMADD_##PRECISION: \
    case RISCV::FADD_##PRECISION: \
    case RISCV::FSUB_##PRECISION: \
    case RISCV::FMUL_##PRECISION: \
    case RISCV::FDIV_##PRECISION: \
    case RISCV::FSQRT_##PRECISION: \
    case RISCV::FSGNJ_##PRECISION: \
    case RISCV::FSGNJN_##PRECISION: \
    case RISCV::FSGNJX_##PRECISION: \
    case RISCV::FMIN_##PRECISION: \
    case RISCV::FMAX_##PRECISION: \
    case RISCV::FCVT_W_##PRECISION: \
    case RISCV::FCVT_WU_##PRECISION: \
    case RISCV::FEQ_##PRECISION: \
    case RISCV::FLT_##PRECISION: \
    case RISCV::FLE_##PRECISION: \
    case RISCV::FCLASS_##PRECISION: \
    case RISCV::FCVT_##PRECISION##_W: \
    case RISCV::FCVT_##PRECISION##_WU: \
    case RISCV::FCVT_L_##PRECISION: \
    case RISCV::FCVT_LU_##PRECISION: \
    case RISCV::FCVT_##PRECISION##_L: \
    case RISCV::FCVT_##PRECISION##_LU:
// clang-format on

inline bool isRVVExt(unsigned Opcode);
inline bool isRVVIntegerWidening(unsigned Opcode);
inline bool isRVVFPWidening(unsigned Opcode);
inline bool isRVVIntegerNarrowing(unsigned Opcode);
inline bool isRVVFPNarrowing(unsigned Opcode);
inline unsigned getRVVExtFactor(unsigned Opcode);

inline size_t getDataElementWidth(unsigned Opcode, unsigned SEW = 0,
                                  unsigned VLENB = 0) {
  if (isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
      isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) {
    return SEW * 2 / CHAR_BIT;
  }
  switch (Opcode) {
  case RISCV::VL1RE8_V:
  case RISCV::VL1RE16_V:
  case RISCV::VL1RE32_V:
  case RISCV::VL1RE64_V:
  case RISCV::VS1R_V:
  case RISCV::VMV1R_V:
    return VLENB;
  case RISCV::VL2RE8_V:
  case RISCV::VL2RE16_V:
  case RISCV::VL2RE32_V:
  case RISCV::VL2RE64_V:
  case RISCV::VS2R_V:
  case RISCV::VMV2R_V:
    return VLENB * 2u;
  case RISCV::VL4RE8_V:
  case RISCV::VL4RE16_V:
  case RISCV::VL4RE32_V:
  case RISCV::VL4RE64_V:
  case RISCV::VS4R_V:
  case RISCV::VMV4R_V:
    return VLENB * 4u;
  case RISCV::VL8RE8_V:
  case RISCV::VL8RE16_V:
  case RISCV::VL8RE32_V:
  case RISCV::VL8RE64_V:
  case RISCV::VS8R_V:
  case RISCV::VMV8R_V:
    return VLENB * 8u;
  case RISCV::VLE8_V:
  case RISCV::VLE8FF_V:
  case RISCV::VSE8_V:
  case RISCV::VSSE8_V:
  case RISCV::VLSE8_V:
  case RISCV::VLM_V:
  case RISCV::VSM_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 8FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 8):
    return 1;
  case RISCV::VLE16_V:
  case RISCV::VLE16FF_V:
  case RISCV::VSE16_V:
  case RISCV::VLSE16_V:
  case RISCV::VSSE16_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 16FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 16):
    return 2;
  case RISCV::VLE32_V:
  case RISCV::VLE32FF_V:
  case RISCV::VSE32_V:
  case RISCV::VLSE32_V:
  case RISCV::VSSE32_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 32FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 32):
    return 4;
  case RISCV::VLE64_V:
  case RISCV::VLE64FF_V:
  case RISCV::VSE64_V:
  case RISCV::VLSE64_V:
  case RISCV::VSSE64_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 64FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 64):
    return 8;
  case RISCV::LD:
  case RISCV::C_LD:
  case RISCV::C_LDSP:
  case RISCV::SD:
  case RISCV::C_SD:
  case RISCV::C_SDSP:
  case ATOMIC_INST_PLACER(RISCV::SC_D):
  case ATOMIC_INST_PLACER(RISCV::LR_D):
  case ATOMIC_INST_PLACER(RISCV::AMOADD_D):
  case ATOMIC_INST_PLACER(RISCV::AMOAND_D):
  case ATOMIC_INST_PLACER(RISCV::AMOOR_D):
  case ATOMIC_INST_PLACER(RISCV::AMOXOR_D):
  case ATOMIC_INST_PLACER(RISCV::AMOSWAP_D):
  case ATOMIC_INST_PLACER(RISCV::AMOMAX_D):
  case ATOMIC_INST_PLACER(RISCV::AMOMAXU_D):
  case ATOMIC_INST_PLACER(RISCV::AMOMIN_D):
  case ATOMIC_INST_PLACER(RISCV::AMOMINU_D):
  case RISCV::FLD:
  case RISCV::C_FLD:
  case RISCV::C_FLDSP:
  case RISCV::FSD:
  case RISCV::C_FSD:
  case RISCV::C_FSDSP:
    return 8;
  case RISCV::LW:
  case RISCV::LWU:
  case RISCV::C_LW:
  case RISCV::C_LWSP:
  case RISCV::SW:
  case RISCV::C_SW:
  case RISCV::C_SWSP:
  case ATOMIC_INST_PLACER(RISCV::SC_W):
  case ATOMIC_INST_PLACER(RISCV::LR_W):
  case ATOMIC_INST_PLACER(RISCV::AMOADD_W):
  case ATOMIC_INST_PLACER(RISCV::AMOAND_W):
  case ATOMIC_INST_PLACER(RISCV::AMOOR_W):
  case ATOMIC_INST_PLACER(RISCV::AMOXOR_W):
  case ATOMIC_INST_PLACER(RISCV::AMOSWAP_W):
  case ATOMIC_INST_PLACER(RISCV::AMOMAX_W):
  case ATOMIC_INST_PLACER(RISCV::AMOMAXU_W):
  case ATOMIC_INST_PLACER(RISCV::AMOMIN_W):
  case ATOMIC_INST_PLACER(RISCV::AMOMINU_W):
  case RISCV::FLW:
  case RISCV::C_FLW:
  case RISCV::C_FLWSP:
  case RISCV::FSW:
  case RISCV::C_FSW:
  case RISCV::C_FSWSP:
    return 4;
  case RISCV::LH:
  case RISCV::LHU:
  case RISCV::SH:
  case RISCV::FLH:
  case RISCV::FSH:
    return 2;
  case RISCV::LB:
  case RISCV::LBU:
  case RISCV::SB:
    return 1;
  default:
    // For vector instructions return one element size.
    return SEW / CHAR_BIT;
  }
}

inline bool isRVV(unsigned Opcode);

inline size_t getLoadStoreNaturalAlignment(unsigned Opcode, unsigned SEW = 0) {
  switch (Opcode) {
  default:
    assert(!isRVV(Opcode));
    return getDataElementWidth(Opcode);
  case RISCV::VLE8_V:
  case RISCV::VSE8_V:
  case RISCV::VLM_V:
  case RISCV::VSM_V:
  case RISCV::VLSE8_V:
  case RISCV::VSSE8_V:
  case RISCV::VLE8FF_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 8FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 8):
  case RISCV::VL1RE8_V:
  case RISCV::VL2RE8_V:
  case RISCV::VL4RE8_V:
  case RISCV::VL8RE8_V:
  case RISCV::VS1R_V:
  case RISCV::VS2R_V:
  case RISCV::VS4R_V:
  case RISCV::VS8R_V:
    return 1;
  case RISCV::VLE16_V:
  case RISCV::VSE16_V:
  case RISCV::VLSE16_V:
  case RISCV::VSSE16_V:
  case RISCV::VLE16FF_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 16FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 16):
  case RISCV::VL1RE16_V:
  case RISCV::VL2RE16_V:
  case RISCV::VL4RE16_V:
  case RISCV::VL8RE16_V:
    return 2;
  case RISCV::VLE32_V:
  case RISCV::VSE32_V:
  case RISCV::VLSE32_V:
  case RISCV::VSSE32_V:
  case RISCV::VLE32FF_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 32FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 32):
  case RISCV::VL1RE32_V:
  case RISCV::VL2RE32_V:
  case RISCV::VL4RE32_V:
  case RISCV::VL8RE32_V:
    return 4;
  case RISCV::VLE64_V:
  case RISCV::VSE64_V:
  case RISCV::VLSE64_V:
  case RISCV::VSSE64_V:
  case RISCV::VLE64FF_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSEG, 64FF):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLSSEG, 64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSSSEG, 64):
  case RISCV::VL1RE64_V:
  case RISCV::VL2RE64_V:
  case RISCV::VL4RE64_V:
  case RISCV::VL8RE64_V:
    return 8;
  case RISCV::VLUXEI8_V:
  case RISCV::VLUXEI16_V:
  case RISCV::VLUXEI32_V:
  case RISCV::VLUXEI64_V:
  case RISCV::VLOXEI8_V:
  case RISCV::VLOXEI16_V:
  case RISCV::VLOXEI32_V:
  case RISCV::VLOXEI64_V:
  case RISCV::VSUXEI8_V:
  case RISCV::VSUXEI16_V:
  case RISCV::VSUXEI32_V:
  case RISCV::VSUXEI64_V:
  case RISCV::VSOXEI8_V:
  case RISCV::VSOXEI16_V:
  case RISCV::VSOXEI32_V:
  case RISCV::VSOXEI64_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I64):
    assert(SEW > 0);
    return SEW / 8;
  }
}

inline bool isScInstr(unsigned Opcode) {
  switch (Opcode) {
  case ATOMIC_INST_PLACER(RISCV::SC_W):
  case ATOMIC_INST_PLACER(RISCV::SC_D):
    return true;
  default:
    return false;
  }
}

inline bool isLrInstr(unsigned Opcode) {
  switch (Opcode) {
  case ATOMIC_INST_PLACER(RISCV::LR_W):
  case ATOMIC_INST_PLACER(RISCV::LR_D):
    return true;
  default:
    return false;
  }
}

#undef ATOMIC_INST_PLACER

inline bool isAtomicAMO(unsigned Opcode) {
  return (Opcode >= RISCV::AMOADD_D && Opcode <= RISCV::AMOXOR_W_RL);
}

// TODO: temporary and brittle classifier, re-implement
inline bool isRVV(unsigned Opcode) {
  return (Opcode >= RISCV::VAADDU_VV && Opcode <= RISCV::VZEXT_VF8);
}

inline bool isFloatingPoint(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
    RV_FLOATING_POINT(D)
    RV_FLOATING_POINT(S)
    RV_FLOATING_POINT(H)
  case RISCV::FMV_X_D:
  case RISCV::FMV_D_X:
  case RISCV::FLD:
  case RISCV::FSD:
  case RISCV::FMV_W_X:
  case RISCV::FMV_X_W:
  case RISCV::FLW:
  case RISCV::FSW:
  case RISCV::FMV_X_H:
  case RISCV::FMV_H_X:
  case RISCV::FLH:
  case RISCV::FSH:
    return true;
  }
}

inline bool isRVVModeSwitch(unsigned Opcode) {
  return (Opcode == RISCV::VSETVL || Opcode == RISCV::VSETVLI ||
          Opcode == RISCV::VSETIVLI);
}

inline size_t getIndexElementWidth(unsigned Opcode) {
  switch (Opcode) {
  case RISCV::VLUXEI8_V:
  case RISCV::VLOXEI8_V:
  case RISCV::VSUXEI8_V:
  case RISCV::VSOXEI8_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I8):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I8):
    return 8;
  case RISCV::VLUXEI16_V:
  case RISCV::VLOXEI16_V:
  case RISCV::VSUXEI16_V:
  case RISCV::VSOXEI16_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I16):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I16):
    return 16;
  case RISCV::VLUXEI32_V:
  case RISCV::VLOXEI32_V:
  case RISCV::VSUXEI32_V:
  case RISCV::VSOXEI32_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I32):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I32):
    return 32;
  case RISCV::VLUXEI64_V:
  case RISCV::VLOXEI64_V:
  case RISCV::VSUXEI64_V:
  case RISCV::VSOXEI64_V:
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLUXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VLOXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSUXSEG, I64):
  case RVV_SEG_EACHFIELD_PLACER(RISCV::VSOXSEG, I64):
    return 64;
  default:
    llvm_unreachable(
        "Invalid load/store opcode: it doesn't have indexes operand");
  }
}

#undef RVV_SEG_EACHFIELD_PLACER

#ifdef RVV_SEG_EACHEEW_PLACER
#error "defined internal-only define RVV_SEG_EACHEEW_PLACER"
#endif
// clang-format off
#define RVV_SEG_EACHEEW_PLACER(BASIC_OPCODE, NSEG, SUFFIX)                     \
  RISCV::BASIC_OPCODE##NSEG##E8##SUFFIX##_V:                                   \
  case RISCV::BASIC_OPCODE##NSEG##E16##SUFFIX##_V:                             \
  case RISCV::BASIC_OPCODE##NSEG##E32##SUFFIX##_V:                             \
  case RISCV::BASIC_OPCODE##NSEG##E64##SUFFIX##_V
// clang-format on

#ifdef RVV_SEG_EACHEIEW_PLACER
#error "defined internal-only define RVV_SEG_EACHEIEW_PLACER"
#endif
// clang-format off
#define RVV_SEG_EACHEIEW_PLACER(BASIC_OPCODE, NSEG)                            \
  RISCV::BASIC_OPCODE##NSEG##EI8_V:                                            \
  case RISCV::BASIC_OPCODE##NSEG##EI16_V:                                      \
  case RISCV::BASIC_OPCODE##NSEG##EI32_V:                                      \
  case RISCV::BASIC_OPCODE##NSEG##EI64_V
// clang-format on

inline size_t getNumFields(unsigned Opcode) {
  switch (Opcode) {
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 2, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 2, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 2, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 2, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 2, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 2):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 2):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 2):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 2):
    return 2;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 3, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 3, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 3, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 3, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 3, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 3):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 3):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 3):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 3):
    return 3;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 4, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 4, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 4, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 4, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 4, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 4):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 4):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 4):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 4):
    return 4;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 5, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 5, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 5, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 5, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 5, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 5):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 5):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 5):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 5):
    return 5;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 6, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 6, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 6, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 6, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 6, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 6):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 6):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 6):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 6):
    return 6;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 7, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 7, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 7, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 7, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 7, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 7):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 7):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 7):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 7):
    return 7;
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 8, ):
  case RVV_SEG_EACHEEW_PLACER(VLSEG, 8, FF):
  case RVV_SEG_EACHEEW_PLACER(VSSEG, 8, ):
  case RVV_SEG_EACHEEW_PLACER(VLSSEG, 8, ):
  case RVV_SEG_EACHEEW_PLACER(VSSSEG, 8, ):
  case RVV_SEG_EACHEIEW_PLACER(VLUXSEG, 8):
  case RVV_SEG_EACHEIEW_PLACER(VLOXSEG, 8):
  case RVV_SEG_EACHEIEW_PLACER(VSUXSEG, 8):
  case RVV_SEG_EACHEIEW_PLACER(VSOXSEG, 8):
    return 8;
  default:
    llvm_unreachable("Invalid segment load/store instruction opcode");
  }
}

#ifdef RVV_SEG_EACHNFIELDS_PLACER
#error "defined internal-only define RVV_SEG_EACHNFIELDS_PLACER"
#endif
// clang-format off
#define RVV_SEG_EACHNFIELDS_PLACER(OPCODE, SUFFIX)                             \
  RVV_SEG_EACHEEW_PLACER(OPCODE, 2, SUFFIX):                                   \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 3, SUFFIX):                              \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 4, SUFFIX):                              \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 5, SUFFIX):                              \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 6, SUFFIX):                              \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 7, SUFFIX):                              \
  case RVV_SEG_EACHEEW_PLACER(OPCODE, 8, SUFFIX)
// clang-format on

#ifdef RVV_INDEX_SEG_EACHNFIELDS_PLACER
#error "defined internal-only define RVV_INDEX_SEG_EACHNFIELDS_PLACER"
#endif
// clang-format off
#define RVV_INDEX_SEG_EACHNFIELDS_PLACER(OPCODE)                               \
  RVV_SEG_EACHEIEW_PLACER(OPCODE, 2):                                          \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 3):                                     \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 4):                                     \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 5):                                     \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 6):                                     \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 7):                                     \
  case RVV_SEG_EACHEIEW_PLACER(OPCODE, 8)
// clang-format on

inline bool isRVVUnitStrideSegLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RVV_SEG_EACHNFIELDS_PLACER(VLSEG, ):
  case RVV_SEG_EACHNFIELDS_PLACER(VLSEG, FF):
  case RVV_SEG_EACHNFIELDS_PLACER(VSSEG, ):
    return true;
  }
}

inline bool isRVVStridedSegLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RVV_SEG_EACHNFIELDS_PLACER(VLSSEG, ):
  case RVV_SEG_EACHNFIELDS_PLACER(VSSSEG, ):
    return true;
  }
}

inline bool isRVVIndexedSegLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RVV_INDEX_SEG_EACHNFIELDS_PLACER(VLUXSEG):
  case RVV_INDEX_SEG_EACHNFIELDS_PLACER(VLOXSEG):
  case RVV_INDEX_SEG_EACHNFIELDS_PLACER(VSUXSEG):
  case RVV_INDEX_SEG_EACHNFIELDS_PLACER(VSOXSEG):
    return true;
  }
}
#undef RVV_SEG_EACHNFIELDS_PLACER
#undef RVV_INDEX_SEG_EACHNFIELDS_PLACER
#undef RVV_SEG_EACHEEW_PLACER
#undef RVV_SEG_EACHEIEW_PLACER

inline bool isRVVWholeRegLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VL1RE8_V:
  case RISCV::VL1RE16_V:
  case RISCV::VL1RE32_V:
  case RISCV::VL1RE64_V:
  case RISCV::VL2RE8_V:
  case RISCV::VL2RE16_V:
  case RISCV::VL2RE32_V:
  case RISCV::VL2RE64_V:
  case RISCV::VL4RE8_V:
  case RISCV::VL4RE16_V:
  case RISCV::VL4RE32_V:
  case RISCV::VL4RE64_V:
  case RISCV::VL8RE8_V:
  case RISCV::VL8RE16_V:
  case RISCV::VL8RE32_V:
  case RISCV::VL8RE64_V:
  case RISCV::VS1R_V:
  case RISCV::VS2R_V:
  case RISCV::VS4R_V:
  case RISCV::VS8R_V:
    return true;
  }
}

// FIXME: add another arch
// No opcodes for SCALL, SBREAK
inline bool isBaseSubset(unsigned Opcode) {
  return (Opcode >= RISCV::ADD && Opcode <= RISCV::ADDW) ||
         (Opcode >= RISCV::AND && Opcode <= RISCV::BNE) ||
         (Opcode >= RISCV::JAL && Opcode <= RISCV::LHU) ||
         (Opcode >= RISCV::LUI && Opcode <= RISCV::LWU) ||
         (Opcode >= RISCV::OR && Opcode <= RISCV::ORI) ||
         (Opcode >= RISCV::SLL && Opcode <= RISCV::SUBW &&
          Opcode != RISCV::SRET) ||
         (Opcode >= RISCV::XOR && Opcode <= RISCV::XORI) ||
         Opcode == RISCV::FENCE || Opcode == RISCV::SB || Opcode == RISCV::SH ||
         Opcode == RISCV::SW || Opcode == RISCV::SD;
}

inline bool isCBaseSubset(unsigned Opcode) {
  return (Opcode >= RISCV::C_ADD && Opcode <= RISCV::C_ANDI) ||
         (Opcode >= RISCV::C_LD && Opcode <= RISCV::C_XOR);
}

inline bool isBaseCFInstr(unsigned Opcode) {
  switch (Opcode) {
  case RISCV::BEQ:
  case RISCV::BNE:
  case RISCV::BLT:
  case RISCV::BGE:
  case RISCV::BLTU:
  case RISCV::BGEU:
  case RISCV::JAL:
  case RISCV::JALR:
    return true;
  default:
    return false;
  }
}

inline bool isLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::LB:
  case RISCV::LBU:
  case RISCV::LH:
  case RISCV::LHU:
  case RISCV::LW:
  case RISCV::LWU:
  case RISCV::LD:
  case RISCV::SB:
  case RISCV::SH:
  case RISCV::SW:
  case RISCV::SD:
    return true;
  }
}

inline bool isFence(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::FENCE:
  case RISCV::FENCE_I:
  case RISCV::FENCE_TSO:
    return true;
  }
}

inline bool isCLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::C_LW:
  case RISCV::C_LD:
  case RISCV::C_LWSP:
  case RISCV::C_LDSP:
  case RISCV::C_SW:
  case RISCV::C_SD:
  case RISCV::C_SWSP:
  case RISCV::C_SDSP:
    return true;
  }
}

inline bool isCSPRelativeLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::C_LWSP:
  case RISCV::C_LDSP:
  case RISCV::C_SWSP:
  case RISCV::C_SDSP:
    return true;
  }
}

inline bool isFPLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::FLH:
  case RISCV::FSH:
  case RISCV::FLW:
  case RISCV::FSW:
  case RISCV::FLD:
  case RISCV::FSD:
    return true;
  }
}

inline bool isCFPLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::C_FLW:
  case RISCV::C_FLD:
  case RISCV::C_FLWSP:
  case RISCV::C_FLDSP:
  case RISCV::C_FSW:
  case RISCV::C_FSD:
  case RISCV::C_FSWSP:
  case RISCV::C_FSDSP:
    return true;
  }
}

inline bool isCFPSPRelativeLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::C_FLWSP:
  case RISCV::C_FLDSP:
  case RISCV::C_FSWSP:
  case RISCV::C_FSDSP:
    return true;
  }
}

inline bool isRVVUnitStrideLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VLE8_V:
  case RISCV::VLE16_V:
  case RISCV::VLE32_V:
  case RISCV::VLE64_V:
  case RISCV::VSE8_V:
  case RISCV::VSE16_V:
  case RISCV::VSE32_V:
  case RISCV::VSE64_V:
    return true;
  }
}

inline bool isRVVUnitStrideFFLoad(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VLE8FF_V:
  case RISCV::VLE16FF_V:
  case RISCV::VLE32FF_V:
  case RISCV::VLE64FF_V:
    return true;
  }
}

inline bool isRVVStridedLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VLSE8_V:
  case RISCV::VLSE16_V:
  case RISCV::VLSE32_V:
  case RISCV::VLSE64_V:
  case RISCV::VSSE8_V:
  case RISCV::VSSE16_V:
  case RISCV::VSSE32_V:
  case RISCV::VSSE64_V:
    return true;
  }
}

inline bool isRVVIndexedLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VLUXEI8_V:
  case RISCV::VLUXEI16_V:
  case RISCV::VLUXEI32_V:
  case RISCV::VLUXEI64_V:
  case RISCV::VLOXEI8_V:
  case RISCV::VLOXEI16_V:
  case RISCV::VLOXEI32_V:
  case RISCV::VLOXEI64_V:
  case RISCV::VSUXEI8_V:
  case RISCV::VSUXEI16_V:
  case RISCV::VSUXEI32_V:
  case RISCV::VSUXEI64_V:
  case RISCV::VSOXEI8_V:
  case RISCV::VSOXEI16_V:
  case RISCV::VSOXEI32_V:
  case RISCV::VSOXEI64_V:
    return true;
  }
}

inline bool isRVVUnitStrideMaskLoadStore(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VLM_V:
  case RISCV::VSM_V:
    return true;
  }
}

inline bool isRVVSetFirstMaskBit(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VMSBF_M:
  case RISCV::VMSOF_M:
  case RISCV::VMSIF_M:
    return true;
  }
}

inline bool isRVVIota(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VIOTA_M:
    return true;
  }
}

inline bool isRVVCompress(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VCOMPRESS_VM:
    return true;
  }
}

inline bool isRVVSlide1Up(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VSLIDE1UP_VX:
  case RISCV::VFSLIDE1UP_VF:
    return true;
  }
}

inline bool isRVVSlideUp(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VSLIDEUP_VX:
  case RISCV::VSLIDEUP_VI:
    return true;
  }
}

inline bool isRVVuseV0RegExplicitly(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VADC_VIM:
  case RISCV::VADC_VVM:
  case RISCV::VADC_VXM:
  case RISCV::VFMERGE_VFM:
  case RISCV::VMADC_VIM:
  case RISCV::VMADC_VVM:
  case RISCV::VMADC_VXM:
  case RISCV::VMERGE_VIM:
  case RISCV::VMERGE_VVM:
  case RISCV::VMERGE_VXM:
  case RISCV::VMSBC_VVM:
  case RISCV::VMSBC_VXM:
  case RISCV::VSBC_VVM:
  case RISCV::VSBC_VXM:
    return true;
  }
}

inline bool isRVVuseV0RegImplicitly(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VFMV_V_F:
  case RISCV::VMV_V_I:
  case RISCV::VMV_V_X:
  case RISCV::VMV_V_V:
    return true;
  }
}

inline bool isRVVWholeRegisterMove(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VMV1R_V:
  case RISCV::VMV2R_V:
  case RISCV::VMV4R_V:
  case RISCV::VMV8R_V:
    return true;
  }
}

inline bool isRVVScalarMove(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VFMV_F_S:
  case RISCV::VFMV_S_F:
  case RISCV::VMV_X_S:
  case RISCV::VMV_S_X:
    return true;
  }
}

inline unsigned getRVVWholeRegisterMoveCount(unsigned Opcode) {
  assert(isRVVWholeRegisterMove(Opcode));
  switch (Opcode) {
  default:
    llvm_unreachable("Not a whole register move instruction");
  case RISCV::VMV1R_V:
    return 1;
  case RISCV::VMV2R_V:
    return 2;
  case RISCV::VMV4R_V:
    return 4;
  case RISCV::VMV8R_V:
    return 8;
  }
}

inline unsigned getRVVWholeRegisterLoadStoreCount(unsigned Opcode) {
  assert(isRVVWholeRegLoadStore(Opcode));
  switch (Opcode) {
  default:
    llvm_unreachable("Not a whole register load/store instruction");
  case RISCV::VL1RE8_V:
  case RISCV::VL1RE16_V:
  case RISCV::VL1RE32_V:
  case RISCV::VL1RE64_V:
  case RISCV::VS1R_V:
    return 1;
  case RISCV::VL2RE8_V:
  case RISCV::VL2RE16_V:
  case RISCV::VL2RE32_V:
  case RISCV::VL2RE64_V:
  case RISCV::VS2R_V:
    return 2;
  case RISCV::VL4RE8_V:
  case RISCV::VL4RE16_V:
  case RISCV::VL4RE32_V:
  case RISCV::VL4RE64_V:
  case RISCV::VS4R_V:
    return 4;
  case RISCV::VL8RE8_V:
  case RISCV::VL8RE16_V:
  case RISCV::VL8RE32_V:
  case RISCV::VL8RE64_V:
  case RISCV::VS8R_V:
    return 8;
  }
}

inline bool isRVVFloatingPoint(unsigned Opcode) {
  // FIXME: We must not rely on opcodes order.
  switch (Opcode) {
  default:
    return (RISCV::VFADD_VF <= Opcode && Opcode <= RISCV::VFWSUB_WV) ||
           (RISCV::VMFEQ_VF <= Opcode && Opcode <= RISCV::VMFNE_VV);
  case RISCV::VFIRST_M:
    return false;
  }
}

inline bool isRVVExt(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VSEXT_VF2:
  case RISCV::VZEXT_VF2:
  case RISCV::VSEXT_VF4:
  case RISCV::VZEXT_VF4:
  case RISCV::VSEXT_VF8:
  case RISCV::VZEXT_VF8:
    return true;
  }
}

inline unsigned getRVVExtFactor(unsigned Opcode) {
  assert(isRVVExt(Opcode));
  switch (Opcode) {
  default:
    llvm_unreachable("Unexpected RVV zext/sext opcode is given");
  case RISCV::VSEXT_VF2:
  case RISCV::VZEXT_VF2:
    return 2u;
  case RISCV::VSEXT_VF4:
  case RISCV::VZEXT_VF4:
    return 4u;
  case RISCV::VSEXT_VF8:
  case RISCV::VZEXT_VF8:
    return 8u;
  }
}

inline Register getBaseRegisterForVRMClass(Register Reg) {
  switch (Reg) {
  default:
    llvm_unreachable("Unexpected Register opcode");
  case RISCV::V0M2:
  case RISCV::V0M4:
  case RISCV::V0M8:
    return RISCV::V0;
  case RISCV::V2M2:
    return RISCV::V2;
  case RISCV::V4M2:
  case RISCV::V4M4:
    return RISCV::V4;
  case RISCV::V6M2:
    return RISCV::V6;
  case RISCV::V8M2:
  case RISCV::V8M4:
  case RISCV::V8M8:
    return RISCV::V8;
  case RISCV::V10M2:
    return RISCV::V10;
  case RISCV::V12M2:
  case RISCV::V12M4:
    return RISCV::V12;
  case RISCV::V14M2:
    return RISCV::V14;
  case RISCV::V16M2:
  case RISCV::V16M4:
  case RISCV::V16M8:
    return RISCV::V16;
  case RISCV::V18M2:
    return RISCV::V18;
  case RISCV::V20M2:
  case RISCV::V20M4:
    return RISCV::V20;
  case RISCV::V22M2:
    return RISCV::V22;
  case RISCV::V24M2:
  case RISCV::V24M4:
  case RISCV::V24M8:
    return RISCV::V24;
  case RISCV::V26M2:
    return RISCV::V26;
  case RISCV::V28M2:
  case RISCV::V28M4:
    return RISCV::V28;
  case RISCV::V30M2:
    return RISCV::V30;
  }
}

inline bool isRVVIntegerWidening(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VWADD_VV:
  case RISCV::VWADDU_VV:
  case RISCV::VWSUB_VV:
  case RISCV::VWSUBU_VV:
  case RISCV::VWMUL_VV:
  case RISCV::VWMULU_VV:
  case RISCV::VWMULSU_VV:
  case RISCV::VWADD_VX:
  case RISCV::VWADDU_VX:
  case RISCV::VWSUB_VX:
  case RISCV::VWSUBU_VX:
  case RISCV::VWMUL_VX:
  case RISCV::VWMULU_VX:
  case RISCV::VWMULSU_VX:
  case RISCV::VWADD_WV:
  case RISCV::VWADDU_WV:
  case RISCV::VWSUB_WV:
  case RISCV::VWSUBU_WV:
  case RISCV::VWADD_WX:
  case RISCV::VWADDU_WX:
  case RISCV::VWSUB_WX:
  case RISCV::VWSUBU_WX:
  case RISCV::VWMACC_VV:
  case RISCV::VWMACCU_VV:
  case RISCV::VWMACCSU_VV:
  case RISCV::VWMACC_VX:
  case RISCV::VWMACCU_VX:
  case RISCV::VWMACCSU_VX:
  case RISCV::VWMACCUS_VX:
  case RISCV::VWREDSUM_VS:
  case RISCV::VWREDSUMU_VS:
    return true;
  }
}

inline bool isRVVIntegerNarrowing(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VNSRA_WI:
  case RISCV::VNSRA_WV:
  case RISCV::VNSRA_WX:
  case RISCV::VNSRL_WI:
  case RISCV::VNSRL_WV:
  case RISCV::VNSRL_WX:
  case RISCV::VNCLIPU_WI:
  case RISCV::VNCLIPU_WV:
  case RISCV::VNCLIPU_WX:
  case RISCV::VNCLIP_WI:
  case RISCV::VNCLIP_WV:
  case RISCV::VNCLIP_WX:
    return true;
  }
}

inline bool isRVVFPWidening(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VFWADD_VV:
  case RISCV::VFWADD_VF:
  case RISCV::VFWADD_WF:
  case RISCV::VFWADD_WV:
  case RISCV::VFWMACC_VV:
  case RISCV::VFWMACC_VF:
  case RISCV::VFWMSAC_VV:
  case RISCV::VFWMSAC_VF:
  case RISCV::VFWMUL_VV:
  case RISCV::VFWMUL_VF:
  case RISCV::VFWNMACC_VV:
  case RISCV::VFWNMACC_VF:
  case RISCV::VFWNMSAC_VV:
  case RISCV::VFWNMSAC_VF:
  case RISCV::VFWSUB_VV:
  case RISCV::VFWSUB_VF:
  case RISCV::VFWSUB_WV:
  case RISCV::VFWSUB_WF:
  case RISCV::VFWREDOSUM_VS:
  case RISCV::VFWREDUSUM_VS:
  case RISCV::VFWCVT_F_F_V:
  case RISCV::VFWCVT_F_XU_V:
  case RISCV::VFWCVT_F_X_V:
  case RISCV::VFWCVT_RTZ_XU_F_V:
  case RISCV::VFWCVT_RTZ_X_F_V:
  case RISCV::VFWCVT_XU_F_V:
  case RISCV::VFWCVT_X_F_V:
    return true;
  }
}

inline bool isRVVSemiWidening(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VWADDU_WV:
  case RISCV::VWSUBU_WV:
  case RISCV::VWADD_WV:
  case RISCV::VWSUB_WV:
  case RISCV::VFWADD_WV:
  case RISCV::VFWSUB_WV:
  case RISCV::VWADDU_WX:
  case RISCV::VWSUBU_WX:
  case RISCV::VWADD_WX:
  case RISCV::VWSUB_WX:
  case RISCV::VFWADD_WF:
  case RISCV::VFWSUB_WF:
    return true;
  }
}

inline bool isRVVFPNarrowing(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VFNCVT_F_F_W:
  case RISCV::VFNCVT_F_XU_W:
  case RISCV::VFNCVT_F_X_W:
  case RISCV::VFNCVT_ROD_F_F_W:
  case RISCV::VFNCVT_RTZ_XU_F_W:
  case RISCV::VFNCVT_RTZ_X_F_W:
  case RISCV::VFNCVT_XU_F_W:
  case RISCV::VFNCVT_X_F_W:
    return true;
  }
}

inline bool isRVVGather(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VRGATHER_VI:
  case RISCV::VRGATHER_VV:
  case RISCV::VRGATHER_VX:
    return true;
  }
}

inline bool isRVVGather16(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VRGATHEREI16_VV:
    return true;
  }
}

inline bool isCall(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::C_JAL:
  case RISCV::C_JALR:
  case RISCV::JAL:
  case RISCV::JALR:
    return true;
  }
}

inline bool isRVVReadsVd(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VFMACC_VV:
  case RISCV::VFNMACC_VV:
  case RISCV::VFNMSAC_VV:
  case RISCV::VFMADD_VV:
  case RISCV::VFNMADD_VV:
  case RISCV::VFMSUB_VV:
  case RISCV::VFNMSUB_VV:
  case RISCV::VFMACC_VF:
  case RISCV::VFNMACC_VF:
  case RISCV::VFNMSAC_VF:
  case RISCV::VFMADD_VF:
  case RISCV::VFNMADD_VF:
  case RISCV::VFMSUB_VF:
  case RISCV::VFNMSUB_VF:
  case RISCV::VWMACCU_VV:
  case RISCV::VWMACC_VV:
  case RISCV::VWMACCSU_VV:
  case RISCV::VFWMACC_VV:
  case RISCV::VFWNMACC_VV:
  case RISCV::VFWMSAC_VV:
  case RISCV::VFWNMSAC_VV:
  case RISCV::VWMACCU_VX:
  case RISCV::VWMACC_VX:
  case RISCV::VWMACCSU_VX:
  case RISCV::VWMACCUS_VX:
  case RISCV::VFWMACC_VF:
  case RISCV::VFWNMACC_VF:
  case RISCV::VFWMSAC_VF:
  case RISCV::VFWNMSAC_VF:
    return true;
  }
}

inline bool isRVVReduction(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VREDSUM_VS:
  case RISCV::VREDMAXU_VS:
  case RISCV::VREDMAX_VS:
  case RISCV::VREDMINU_VS:
  case RISCV::VREDMIN_VS:
  case RISCV::VREDAND_VS:
  case RISCV::VREDOR_VS:
  case RISCV::VREDXOR_VS:
  case RISCV::VFREDOSUM_VS:
  case RISCV::VFREDUSUM_VS:
  case RISCV::VFREDMAX_VS:
  case RISCV::VFREDMIN_VS:
  case RISCV::VWREDSUMU_VS:
  case RISCV::VWREDSUM_VS:
  case RISCV::VFWREDOSUM_VS:
  case RISCV::VFWREDUSUM_VS:
    return true;
  }
}

inline bool isRVVWidening(unsigned Opcode) {
  return isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode);
}

inline bool isRVVNarrowing(unsigned Opcode) {
  return isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode);
}

inline bool isRVVMaskLogical(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::VMAND_MM:
  case RISCV::VMNAND_MM:
  case RISCV::VMANDN_MM:
  case RISCV::VMXOR_MM:
  case RISCV::VMOR_MM:
  case RISCV::VMNOR_MM:
  case RISCV::VMORN_MM:
  case RISCV::VMXNOR_MM:
    return true;
  }
}

inline bool isRVVMaskProducing(unsigned Opcode) {
  switch (Opcode) {
  default:
    return isRVVMaskLogical(Opcode) || isRVVSetFirstMaskBit(Opcode);
  case RISCV::VMSEQ_VV:
  case RISCV::VMSNE_VV:
  case RISCV::VMSLTU_VV:
  case RISCV::VMSLT_VV:
  case RISCV::VMSLEU_VV:
  case RISCV::VMSLE_VV:
  case RISCV::VMFEQ_VV:
  case RISCV::VMFNE_VV:
  case RISCV::VMFLT_VV:
  case RISCV::VMFLE_VV:
  case RISCV::VMSEQ_VX:
  case RISCV::VMSNE_VX:
  case RISCV::VMSLTU_VX:
  case RISCV::VMSLT_VX:
  case RISCV::VMSLEU_VX:
  case RISCV::VMSLE_VX:
  case RISCV::VMSGTU_VX:
  case RISCV::VMSGT_VX:
  case RISCV::VMFEQ_VF:
  case RISCV::VMFNE_VF:
  case RISCV::VMFLT_VF:
  case RISCV::VMFLE_VF:
  case RISCV::VMFGT_VF:
  case RISCV::VMFGE_VF:
  case RISCV::VMSEQ_VI:
  case RISCV::VMSNE_VI:
  case RISCV::VMSLEU_VI:
  case RISCV::VMSLE_VI:
  case RISCV::VMSGTU_VI:
  case RISCV::VMSGT_VI:
  case RISCV::VMADC_VVM:
  case RISCV::VMSBC_VVM:
  case RISCV::VMADC_VXM:
  case RISCV::VMSBC_VXM:
  case RISCV::VMADC_VIM:
    return true;
  }
}

std::pair<unsigned, bool> computeDecodedEMUL(unsigned SEW, unsigned EEW,
                                             RISCVII::VLMUL LMUL);

inline unsigned getRVVRegGroupSize(unsigned SEW, unsigned EEW,
                                   RISCVII::VLMUL LMUL) {
  if (EEW == 1)
    return 1;
  auto [EMUL, EMULIsFractional] = computeDecodedEMUL(SEW, EEW, LMUL);
  if (EMULIsFractional)
    return 1;
  return EMUL;
}

inline unsigned getStoreOpcode(unsigned ValueSizeInBits) {
  switch (ValueSizeInBits) {
  default:
    llvm_unreachable(
        "Store instruction for the given value size in bits doesn't exist");
  case 8:
    return RISCV::SB;
  case 16:
    return RISCV::SH;
  case 32:
    return RISCV::SW;
  case 64:
    return RISCV::SD;
  }
}

inline bool isRVVWholeRegisterInstr(unsigned Opcode) {
  return isRVVWholeRegisterMove(Opcode) || isRVVWholeRegLoadStore(Opcode);
}

inline size_t getRVVWholeRegisterCount(unsigned Opcode) {
  if (isRVVWholeRegisterMove(Opcode))
    return getRVVWholeRegisterMoveCount(Opcode);
  if (isRVVWholeRegLoadStore(Opcode))
    return getRVVWholeRegisterLoadStoreCount(Opcode);
  llvm_unreachable("Not a whole register instruction");
}

} // namespace snippy
} // namespace llvm
