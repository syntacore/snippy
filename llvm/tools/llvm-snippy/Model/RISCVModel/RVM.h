//===-- RVM.h ---------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define RVMAPI_ENTRY_POINT_SYMBOL RVMVTable
#define RVMAPI_VERSION_SYMBOL RVMInterfaceVersion
#define RVMAPI_CURRENT_INTERFACE_VERSION 9u

typedef uint64_t RVMRegT;

typedef struct RVMState RVMState;
typedef struct RVMCallbackHandler RVMCallbackHandler;

#define RVM_API_INTERNAL_STDEXT_FLAG(EXT_LITERAL)                              \
  (1ull << ((EXT_LITERAL) - 'A'))
typedef enum {
  RVM_MISA_A = RVM_API_INTERNAL_STDEXT_FLAG('A'),
  RVM_MISA_B = RVM_API_INTERNAL_STDEXT_FLAG('B'),
  RVM_MISA_C = RVM_API_INTERNAL_STDEXT_FLAG('C'),
  RVM_MISA_D = RVM_API_INTERNAL_STDEXT_FLAG('D'),
  RVM_MISA_E = RVM_API_INTERNAL_STDEXT_FLAG('E'),
  RVM_MISA_F = RVM_API_INTERNAL_STDEXT_FLAG('F'),
  RVM_MISA_G = RVM_API_INTERNAL_STDEXT_FLAG('G'),
  RVM_MISA_H = RVM_API_INTERNAL_STDEXT_FLAG('H'),
  RVM_MISA_I = RVM_API_INTERNAL_STDEXT_FLAG('I'),
  RVM_MISA_J = RVM_API_INTERNAL_STDEXT_FLAG('J'),
  RVM_MISA_K = RVM_API_INTERNAL_STDEXT_FLAG('K'),
  RVM_MISA_L = RVM_API_INTERNAL_STDEXT_FLAG('L'),
  RVM_MISA_M = RVM_API_INTERNAL_STDEXT_FLAG('M'),
  RVM_MISA_N = RVM_API_INTERNAL_STDEXT_FLAG('N'),
  RVM_MISA_O = RVM_API_INTERNAL_STDEXT_FLAG('O'),
  RVM_MISA_P = RVM_API_INTERNAL_STDEXT_FLAG('P'),
  RVM_MISA_Q = RVM_API_INTERNAL_STDEXT_FLAG('Q'),
  RVM_MISA_R = RVM_API_INTERNAL_STDEXT_FLAG('R'),
  RVM_MISA_S = RVM_API_INTERNAL_STDEXT_FLAG('S'),
  RVM_MISA_T = RVM_API_INTERNAL_STDEXT_FLAG('T'),
  RVM_MISA_U = RVM_API_INTERNAL_STDEXT_FLAG('U'),
  RVM_MISA_V = RVM_API_INTERNAL_STDEXT_FLAG('V'),
  RVM_MISA_W = RVM_API_INTERNAL_STDEXT_FLAG('W'),
  RVM_MISA_X = RVM_API_INTERNAL_STDEXT_FLAG('X'),
  RVM_MISA_Y = RVM_API_INTERNAL_STDEXT_FLAG('Y'),
  RVM_MISA_Z = RVM_API_INTERNAL_STDEXT_FLAG('Z'),
} RVMMisaExt;
#undef RVM_API_INTERNAL_STDEXT_FLAG

typedef enum {
  RVM_ZEXT_FH = 1ull << 0,
  RVM_ZEXT_BA = 1ull << 1,
  RVM_ZEXT_BB = 1ull << 2,
  RVM_ZEXT_BC = 1ull << 3,
  RVM_ZEXT_BS = 1ull << 4,
  RVM_ZEXT_BITMANIP = RVM_ZEXT_BA | RVM_ZEXT_BB | RVM_ZEXT_BC | RVM_ZEXT_BS,
  RVM_ZEXT_BKB = 1ull << 5,
  RVM_ZEXT_BKC = 1ull << 6,
  RVM_ZEXT_BKX = 1ull << 7,
  RVM_ZEXT_KND = 1ull << 8,
  RVM_ZEXT_KNE = 1ull << 9,
  RVM_ZEXT_KNH = 1ull << 10,
  RVM_ZEXT_KSED = 1ull << 11,
  RVM_ZEXT_KSH = 1ull << 12,
  RVM_ZEXT_KR = 1ull << 13,
  RVM_ZEXT_KN = 1ull << 14,
  RVM_ZEXT_KS = 1ull << 15,
  RVM_ZEXT_K = 1ull << 16,
  RVM_ZEXT_KT = 1ull << 17,
} RVMZExt;

typedef enum {
  RVM_MSTATUS_VS_FIELD_OFFSET = 9,
  RVM_MSTATUS_FS_FIELD_OFFSET = 13,
} RVMMStatusFields;

struct RVMConfig;
typedef struct RVMConfig RVMConfig;

RVMState *rvm_modelCreate(const RVMConfig *config);
void rvm_modelDestroy(RVMState *State);

const RVMConfig *rvm_getModelConfig(const RVMState *State);

int rvm_executeInstr(RVMState *State);

void rvm_readMem(const RVMState *State, uint64_t Addr, size_t Count,
                 char *Data);
void rvm_writeMem(RVMState *State, uint64_t Addr, size_t Count,
                  const char *Data);

uint64_t rvm_readPC(const RVMState *State);
void rvm_setPC(RVMState *State, uint64_t NewPC);

typedef enum {
  RVM_X_REG_0 = 0,
  RVM_X_REG_1,
  RVM_X_REG_2,
  RVM_X_REG_3,
  RVM_X_REG_4,
  RVM_X_REG_5,
  RVM_X_REG_6,
  RVM_X_REG_7,
  RVM_X_REG_8,
  RVM_X_REG_9,
  RVM_X_REG_10,
  RVM_X_REG_11,
  RVM_X_REG_12,
  RVM_X_REG_13,
  RVM_X_REG_14,
  RVM_X_REG_15,
  RVM_X_REG_16,
  RVM_X_REG_17,
  RVM_X_REG_18,
  RVM_X_REG_19,
  RVM_X_REG_20,
  RVM_X_REG_21,
  RVM_X_REG_22,
  RVM_X_REG_23,
  RVM_X_REG_24,
  RVM_X_REG_25,
  RVM_X_REG_26,
  RVM_X_REG_27,
  RVM_X_REG_28,
  RVM_X_REG_29,
  RVM_X_REG_30,
  RVM_X_REG_31,
} RVMXReg;

RVMRegT rvm_readXReg(const RVMState *State, RVMXReg Reg);
void rvm_setXReg(RVMState *State, RVMXReg Reg, RVMRegT Value);

typedef enum {
  RVM_F_REG_0 = 0,
  RVM_F_REG_1,
  RVM_F_REG_2,
  RVM_F_REG_3,
  RVM_F_REG_4,
  RVM_F_REG_5,
  RVM_F_REG_6,
  RVM_F_REG_7,
  RVM_F_REG_8,
  RVM_F_REG_9,
  RVM_F_REG_10,
  RVM_F_REG_11,
  RVM_F_REG_12,
  RVM_F_REG_13,
  RVM_F_REG_14,
  RVM_F_REG_15,
  RVM_F_REG_16,
  RVM_F_REG_17,
  RVM_F_REG_18,
  RVM_F_REG_19,
  RVM_F_REG_20,
  RVM_F_REG_21,
  RVM_F_REG_22,
  RVM_F_REG_23,
  RVM_F_REG_24,
  RVM_F_REG_25,
  RVM_F_REG_26,
  RVM_F_REG_27,
  RVM_F_REG_28,
  RVM_F_REG_29,
  RVM_F_REG_30,
  RVM_F_REG_31,
} RVMFReg;

RVMRegT rvm_readFReg(const RVMState *State, RVMFReg Reg);
void rvm_setFReg(RVMState *State, RVMFReg Reg, RVMRegT Value);

typedef enum {
  // RISC-V privileged spec, tables 2.2 - 2.6
  RVM_CSR_FFLAGS = 0x001,
  RVM_CSR_FRM = 0x002,
  RVM_CSR_FCSR = 0x003,
  RVM_CSR_CYCLE = 0xC00,
  RVM_CSR_TIME = 0xC01,
  RVM_CSR_INSTRET = 0xC02,
  RVM_CSR_CYCLEH = 0xC80,
  RVM_CSR_TIMEH = 0xC81,
  RVM_CSR_INSTRETH = 0xC82,
  RVM_CSR_MSTATUS = 0x300,
  RVM_CSR_MISA = 0x301,
  RVM_CSR_MTVEC = 0x305,
  RVM_CSR_MCAUSE = 0x342,

  // github.com/riscv/riscv-v-spec/blob/master/v-spec.adoc#vector-extension-programmers-model
  RVM_CSR_VSTART = 0x008,
  RVM_CSR_VXSAT = 0x009,
  RVM_CSR_VXRM = 0x00A,
  RVM_CST_VCSR = 0x00F,
  RVM_CSR_VL = 0xC20,
  RVM_CSR_VTYPE = 0xC21,
  RVM_CSR_VLENB = 0xC22,
} RVMCSR;

RVMRegT rvm_readCSRReg(const RVMState *State, unsigned Reg);
void rvm_setCSRReg(RVMState *State, unsigned Reg, RVMRegT Value);

typedef enum {
  RVM_V_REG_0 = 0,
  RVM_V_REG_1,
  RVM_V_REG_2,
  RVM_V_REG_3,
  RVM_V_REG_4,
  RVM_V_REG_5,
  RVM_V_REG_6,
  RVM_V_REG_7,
  RVM_V_REG_8,
  RVM_V_REG_9,
  RVM_V_REG_10,
  RVM_V_REG_11,
  RVM_V_REG_12,
  RVM_V_REG_13,
  RVM_V_REG_14,
  RVM_V_REG_15,
  RVM_V_REG_16,
  RVM_V_REG_17,
  RVM_V_REG_18,
  RVM_V_REG_19,
  RVM_V_REG_20,
  RVM_V_REG_21,
  RVM_V_REG_22,
  RVM_V_REG_23,
  RVM_V_REG_24,
  RVM_V_REG_25,
  RVM_V_REG_26,
  RVM_V_REG_27,
  RVM_V_REG_28,
  RVM_V_REG_29,
  RVM_V_REG_30,
  RVM_V_REG_31,
} RVMVReg;

int rvm_readVReg(const RVMState *State, RVMVReg Reg, char *Data,
                 size_t MaxSize);
int rvm_setVReg(RVMState *State, RVMVReg Reg, const char *Data,
                size_t DataSize);

void rvm_logMessage(const char *Message);

int rvm_queryCallbackSupportPresent();

typedef void (*MemUpdateCallbackTy)(RVMCallbackHandler *, uint64_t Addr,
                                    const char *Data, size_t Size);
typedef void (*XRegUpdateCallbackTy)(RVMCallbackHandler *, RVMXReg Reg,
                                     RVMRegT Value);
typedef void (*FRegUpdateCallbackTy)(RVMCallbackHandler *, RVMFReg Reg,
                                     RVMRegT Value);
typedef void (*VRegUpdateCallbackTy)(RVMCallbackHandler *, RVMVReg Reg,
                                     const char *Data, size_t Size);
typedef void (*PCUpdateCallbackTy)(RVMCallbackHandler *, uint64_t PC);

struct RVMConfig {
  uint64_t RomStart;
  uint64_t RomSize;
  uint64_t RamStart;
  uint64_t RamSize;
  int RV64;
  uint64_t MisaExt;
  uint64_t ZExt;
  uint64_t XExt;
  unsigned VLEN;
  int EnableMisalignedAccess;
  const char *LogFilePath;
  const char *PluginInfo;
  RVMCallbackHandler *CallbackHandler;
  MemUpdateCallbackTy MemUpdateCallback;
  XRegUpdateCallbackTy XRegUpdateCallback;
  FRegUpdateCallbackTy FRegUpdateCallback;
  VRegUpdateCallbackTy VRegUpdateCallback;
  PCUpdateCallbackTy PCUpdateCallback;
  int ChangeMaskAgnosticElems;
  int ChangeTailAgnosticElems;
};

typedef RVMState *(*rvm_modelCreate_t)(const RVMConfig *);
typedef void (*rvm_modelDestroy_t)(RVMState *);
typedef const RVMConfig *(*rvm_getModelConfig_t)(const RVMState *);
typedef int (*rvm_executeInstr_t)(RVMState *);
typedef void (*rvm_readMem_t)(const RVMState *, uint64_t, size_t, char *);
typedef void (*rvm_writeMem_t)(RVMState *, uint64_t, size_t, const char *);
typedef uint64_t (*rvm_readPC_t)(const RVMState *);
typedef void (*rvm_setPC_t)(RVMState *, uint64_t);
typedef RVMRegT (*rvm_readXReg_t)(const RVMState *, RVMXReg);
typedef void (*rvm_setXReg_t)(RVMState *, RVMXReg, RVMRegT);
typedef RVMRegT (*rvm_readFReg_t)(const RVMState *, RVMFReg);
typedef void (*rvm_setFReg_t)(RVMState *, RVMFReg, RVMRegT);
typedef RVMRegT (*rvm_readCSRReg_t)(const RVMState *, unsigned);
typedef void (*rvm_setCSRReg_t)(RVMState *, unsigned, RVMRegT);
typedef int (*rvm_readVReg_t)(const RVMState *, RVMVReg, char *, size_t);
typedef int (*rvm_setVReg_t)(RVMState *, RVMVReg, const char *, size_t);
typedef void (*rvm_logMessage_t)(const char *);
typedef int (*rvm_queryCallbackSupportPresent_t)();

#ifdef __cplusplus
}
#endif // __cplusplus
