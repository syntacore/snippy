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
#define RVMAPI_CURRENT_INTERFACE_VERSION 20u

typedef uint64_t RVMRegT;

typedef struct RVMState RVMState;
typedef struct RVMCallbackHandler RVMCallbackHandler;

#ifdef RVM_FOR_EACH_MISA_EXT
#error RVM_FOR_EACH_MISA_EXT should not be defined at this point
#else
// Order is important here. See "36.11 Subset Naming Convention"
#define RVM_FOR_EACH_MISA_EXT(MACRO)                                           \
  MACRO(RVM_MISA_M, M)                                                         \
  MACRO(RVM_MISA_A, A)                                                         \
  MACRO(RVM_MISA_F, F)                                                         \
  MACRO(RVM_MISA_D, D)                                                         \
  MACRO(RVM_MISA_Q, Q)                                                         \
  MACRO(RVM_MISA_L, L)                                                         \
  MACRO(RVM_MISA_C, C)                                                         \
  MACRO(RVM_MISA_B, B)                                                         \
  MACRO(RVM_MISA_T, T)                                                         \
  MACRO(RVM_MISA_P, P)                                                         \
  MACRO(RVM_MISA_V, V)                                                         \
  MACRO(RVM_MISA_H, H)                                                         \
  MACRO(RVM_MISA_E, E)                                                         \
  MACRO(RVM_MISA_G, G)                                                         \
  MACRO(RVM_MISA_I, I)                                                         \
  MACRO(RVM_MISA_J, J)                                                         \
  MACRO(RVM_MISA_K, K)                                                         \
  MACRO(RVM_MISA_N, N)                                                         \
  MACRO(RVM_MISA_O, O)                                                         \
  MACRO(RVM_MISA_R, R)                                                         \
  MACRO(RVM_MISA_S, S)                                                         \
  MACRO(RVM_MISA_U, U)                                                         \
  MACRO(RVM_MISA_W, W)                                                         \
  MACRO(RVM_MISA_X, X)                                                         \
  MACRO(RVM_MISA_Y, Y)                                                         \
  MACRO(RVM_MISA_Z, Z)
#endif
#ifdef RVM_DEFINE_ENUM_CASE
#error RVM_DEFINE_ENUM_CASE should not be defined at this point
#else
#define RVM_DEFINE_ENUM_CASE(Name, name) Name,
#endif
typedef enum {
  RVM_FOR_EACH_MISA_EXT(RVM_DEFINE_ENUM_CASE) RVM_MISA_NUMBER
} RVMMisaExt;

#ifdef RVM_FOR_EACH_ZEXT
#error RVM_FOR_EACH_ZEXT should not be defined at this point
#else
#define RVM_FOR_EACH_ZEXT(MACRO)                                               \
  MACRO(RVM_ZEXT_ICSR, icsr)                                                   \
  MACRO(RVM_ZEXT_IFENCEI, ifencei)                                             \
  MACRO(RVM_ZEXT_FH, fh)                                                       \
  MACRO(RVM_ZEXT_FHMIN, fhmin)                                                 \
  MACRO(RVM_ZEXT_BA, ba)                                                       \
  MACRO(RVM_ZEXT_BB, bb)                                                       \
  MACRO(RVM_ZEXT_BC, bc)                                                       \
  MACRO(RVM_ZEXT_BS, bs)                                                       \
  MACRO(RVM_ZEXT_BITMANIP, bitmanip)                                           \
  MACRO(RVM_ZEXT_BKB, bkb)                                                     \
  MACRO(RVM_ZEXT_BKC, bkc)                                                     \
  MACRO(RVM_ZEXT_BKX, bkx)                                                     \
  MACRO(RVM_ZEXT_VKB, vkb)                                                     \
  MACRO(RVM_ZEXT_VBB, vbb)                                                     \
  MACRO(RVM_ZEXT_VBC, vbc)                                                     \
  MACRO(RVM_ZEXT_VKNED, vkned)                                                 \
  MACRO(RVM_ZEXT_VKNHA, vknha)                                                 \
  MACRO(RVM_ZEXT_VKNHB, vknhb)                                                 \
  MACRO(RVM_ZEXT_VKG, vkg)                                                     \
  MACRO(RVM_ZEXT_VKSED, vksed)                                                 \
  MACRO(RVM_ZEXT_VKSH, vksh)                                                   \
  MACRO(RVM_ZEXT_VKT, vkt)                                                     \
  MACRO(RVM_ZEXT_VKN, vkn)                                                     \
  MACRO(RVM_ZEXT_VKNC, vknc)                                                   \
  MACRO(RVM_ZEXT_VKNG, vkng)                                                   \
  MACRO(RVM_ZEXT_VKS, vks)                                                     \
  MACRO(RVM_ZEXT_VKSC, vksc)                                                   \
  MACRO(RVM_ZEXT_VKSG, vksg)                                                   \
  MACRO(RVM_ZEXT_KND, knd)                                                     \
  MACRO(RVM_ZEXT_KNE, kne)                                                     \
  MACRO(RVM_ZEXT_KNH, knh)                                                     \
  MACRO(RVM_ZEXT_KSED, ksed)                                                   \
  MACRO(RVM_ZEXT_KSH, ksh)                                                     \
  MACRO(RVM_ZEXT_KR, kr)                                                       \
  MACRO(RVM_ZEXT_KN, kn)                                                       \
  MACRO(RVM_ZEXT_KS, ks)                                                       \
  MACRO(RVM_ZEXT_K, k)                                                         \
  MACRO(RVM_ZEXT_KT, kt)
#endif

typedef enum {
  RVM_FOR_EACH_ZEXT(RVM_DEFINE_ENUM_CASE) RVM_ZEXT_NUMBER
} RVMZExt;

#ifdef RVM_FOR_EACH_XEXT
#error RVM_FOR_EACH_XEXT should not be defined at this point
#else
#define RVM_FOR_EACH_XEXT(MACRO)
#endif

typedef enum {
  RVM_FOR_EACH_XEXT(RVM_DEFINE_ENUM_CASE) RVM_XEXT_NUMBER
} RVMXExt;
#undef RVM_DEFINE_ENUM_CASE

typedef struct RVMExtDescriptior {
  size_t ZExtSize; // Set this to sizeof(ZExt)
  size_t XExtSize; // Set this to sizeof(XExt)
  char ZExt[RVM_ZEXT_NUMBER];
  char XExt[RVM_XEXT_NUMBER];
  char MisaExt[RVM_MISA_NUMBER];
} RVMExtDescriptor;

typedef enum {
  RVM_MSTATUS_VS_FIELD_OFFSET = 9,
  RVM_MSTATUS_FS_FIELD_OFFSET = 13,
} RVMMStatusFields;

struct RVMConfig;
typedef struct RVMConfig RVMConfig;

typedef enum { NON_STOP = 0, STOP_BY_PC } RVMStopMode;

typedef enum {
  // Simulator stepped successully, no additional event happened.
  MODEL_SUCCESS,
  // Simulator got ebreak or instruction with "StopPC".
  MODEL_FINISH,
  // Simulator got some kind of exception.
  MODEL_EXCEPTION,
} RVMSimExecStatus;

RVMState *rvm_modelCreate(const RVMConfig *config);
void rvm_modelDestroy(RVMState *State);

const RVMConfig *rvm_getModelConfig(const RVMState *State);

RVMSimExecStatus rvm_executeInstr(RVMState *State);

void rvm_readMem(const RVMState *State, uint64_t Addr, size_t Count,
                 char *Data);
void rvm_writeMem(RVMState *State, uint64_t Addr, size_t Count,
                  const char *Data);

void rvm_setStopMode(RVMState *State, RVMStopMode Mode);

void rvm_setStopPC(RVMState *State, uint64_t Addr);

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
  RVM_CSR_MEPC = 0x341,
  RVM_CSR_MCAUSE = 0x342,
  RVM_CSR_MTVAL = 0x343,

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

int rvm_queryCallbackSupportPresent(void);

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
  unsigned VLEN;
  int EnableMisalignedAccess;
  RVMStopMode Mode;
  uint64_t StopAddr;
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
  RVMExtDescriptor Extensions;
};

typedef RVMState *(*rvm_modelCreate_t)(const RVMConfig *);
typedef void (*rvm_modelDestroy_t)(RVMState *);
typedef const RVMConfig *(*rvm_getModelConfig_t)(const RVMState *);
typedef RVMSimExecStatus (*rvm_executeInstr_t)(RVMState *);
typedef void (*rvm_readMem_t)(const RVMState *, uint64_t, size_t, char *);
typedef void (*rvm_writeMem_t)(RVMState *, uint64_t, size_t, const char *);
typedef uint64_t (*rvm_readPC_t)(const RVMState *);
typedef void (*rvm_setPC_t)(RVMState *, uint64_t);
typedef RVMRegT (*rvm_readXReg_t)(const RVMState *, RVMXReg);
typedef void (*rvm_setXReg_t)(RVMState *, RVMXReg, RVMRegT);
typedef RVMRegT (*rvm_readFReg_t)(const RVMState *, RVMFReg);
typedef void (*rvm_setFReg_t)(RVMState *, RVMFReg, RVMRegT);
typedef void (*rvm_setStopMode_t)(RVMState *, RVMStopMode);
typedef void (*rvm_setStopPC_t)(RVMState *, uint64_t);
typedef RVMRegT (*rvm_readCSRReg_t)(const RVMState *, unsigned);
typedef void (*rvm_setCSRReg_t)(RVMState *, unsigned, RVMRegT);
typedef int (*rvm_readVReg_t)(const RVMState *, RVMVReg, char *, size_t);
typedef int (*rvm_setVReg_t)(RVMState *, RVMVReg, const char *, size_t);
typedef void (*rvm_logMessage_t)(const char *);
typedef int (*rvm_queryCallbackSupportPresent_t)(void);

#ifdef __cplusplus
}
#endif // __cplusplus
