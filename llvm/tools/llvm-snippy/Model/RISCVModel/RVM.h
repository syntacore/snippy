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

/** @file RVM.h Contains C interface for RISC-V model
 *
 * For a model to implement this interface it should implement all of the
 * functions declared below.
 *
 * Users of this interface are advised to use C++ API defined in @ref RVM.hpp
 */
#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define RVMAPI_ENTRY_POINT_SYMBOL RVMVTable
#define RVMAPI_VERSION_SYMBOL RVMInterfaceVersion
#define RVMAPI_CURRENT_INTERFACE_VERSION 35u
#if defined(__has_c_attribute)
#if __has_c_attribute(nodiscard)
#define RVM_NODISCARD [[nodiscard]]
#define RVM_NODISCARD_ENUM [[nodiscard]]
#endif
#endif
#if !defined(RVM_NODISCARD_ENUM)
#define RVM_NODISCARD_ENUM
#endif
#if !defined(RVM_NODISCARD)
#if defined(__GNUC__) || defined(__clang__)
#define RVM_NODISCARD __attribute__((warn_unused_result))
#else
#define RVM_NODISCARD
#endif
#endif

/** @brief Type used to store register values as bits. */
typedef uint64_t RVMRegT;

/** @brief Model state (instance) */
typedef struct RVMState RVMState;
/** @brief Handler for all model callbacks
 *
 * Users of the callbacks should define this class into something that allows to
 * pass additional context to other callbacks.
 *
 * For example we want to log every register update and append to the message a
 * custom string. In this case we would define RVMCallbackHandler into struct
 * that stores a string inside it. And then we define XRegUpdateCallback (with
 * type @ref XRegUpdateCallbackTy) as function that accepts this
 * RVMCallbackHandler and new register value and then prints first our custom
 * message from RVMCallbackHandler and later actual register update info.
 *
 * Another useful example of RVMCallbackHandler would be a class that stores a
 * set of different functions that we want to be called when register gets
 * updated and calling all of them from @ref XRegUpdateCallback function
 */
typedef struct RVMCallbackHandler RVMCallbackHandler;

#ifdef RVM_FOR_EACH_MISA_EXT
#error RVM_FOR_EACH_MISA_EXT should not be defined at this point
#else
/** @brief Standard MISA extension list
 *
 * Order is important here. See "36.11 Subset Naming Convention"
 *
 */
#define RVM_FOR_EACH_MISA_EXT(MACRO)                                           \
  MACRO(RVM_MISA_I, i)                                                         \
  MACRO(RVM_MISA_E, e)                                                         \
  MACRO(RVM_MISA_G, g)                                                         \
  MACRO(RVM_MISA_M, m)                                                         \
  MACRO(RVM_MISA_A, a)                                                         \
  MACRO(RVM_MISA_F, f)                                                         \
  MACRO(RVM_MISA_D, d)                                                         \
  MACRO(RVM_MISA_Q, q)                                                         \
  MACRO(RVM_MISA_L, l)                                                         \
  MACRO(RVM_MISA_C, c)                                                         \
  MACRO(RVM_MISA_B, b)                                                         \
  MACRO(RVM_MISA_T, t)                                                         \
  MACRO(RVM_MISA_P, p)                                                         \
  MACRO(RVM_MISA_V, v)                                                         \
  MACRO(RVM_MISA_H, h)                                                         \
  MACRO(RVM_MISA_J, j)                                                         \
  MACRO(RVM_MISA_K, k)                                                         \
  MACRO(RVM_MISA_N, n)                                                         \
  MACRO(RVM_MISA_O, o)                                                         \
  MACRO(RVM_MISA_R, r)                                                         \
  MACRO(RVM_MISA_S, s)                                                         \
  MACRO(RVM_MISA_U, u)                                                         \
  MACRO(RVM_MISA_W, w)                                                         \
  MACRO(RVM_MISA_X, x)                                                         \
  MACRO(RVM_MISA_Y, y)                                                         \
  MACRO(RVM_MISA_Z, z)
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
// Only append to this list to keep ABI compatible when new extensions are
// added.
#define RVM_FOR_EACH_ZEXT(MACRO)                                               \
  MACRO(RVM_ZEXT_ICSR, icsr)                                                   \
  MACRO(RVM_ZEXT_IFENCEI, ifencei)                                             \
  MACRO(RVM_ZEXT_ICOND, icond)                                                 \
  MACRO(RVM_ZEXT_ICBOM, icbom)                                                 \
  MACRO(RVM_ZEXT_ICBOZ, icboz)                                                 \
  MACRO(RVM_ZEXT_ICNTR, icntr)                                                 \
  MACRO(RVM_ZEXT_ICBOP, icbop)                                                 \
  MACRO(RVM_ZEXT_IMOP, imop)                                                   \
  MACRO(RVM_ZEXT_ILSD, ilsd)                                                   \
  MACRO(RVM_ZEXT_IHPM, ihpm)                                                   \
  MACRO(RVM_ZEXT_IHINTNTL, ihintntl)                                           \
  MACRO(RVM_ZEXT_IHINTPAUSE, ihintpause)                                       \
  MACRO(RVM_ZEXT_ICFISS, icfiss)                                               \
  MACRO(RVM_ZEXT_ICFILP, icfilp)                                               \
  MACRO(RVM_ZEXT_AAMO, aamo)                                                   \
  MACRO(RVM_ZEXT_ABHA, abha)                                                   \
  MACRO(RVM_ZEXT_ACAS, acas)                                                   \
  MACRO(RVM_ZEXT_ALASR, alasr)                                                 \
  MACRO(RVM_ZEXT_ALRSC, alrsc)                                                 \
  MACRO(RVM_ZEXT_AWRS, awrs)                                                   \
  MACRO(RVM_ZEXT_CMOP, cmop)                                                   \
  MACRO(RVM_ZEXT_CMP, cmp)                                                     \
  MACRO(RVM_ZEXT_CMT, cmt)                                                     \
  MACRO(RVM_ZEXT_CE, ce)                                                       \
  MACRO(RVM_ZEXT_CA, ca)                                                       \
  MACRO(RVM_ZEXT_CB, cb)                                                       \
  MACRO(RVM_ZEXT_CF, cf)                                                       \
  MACRO(RVM_ZEXT_CD, cd)                                                       \
  MACRO(RVM_ZEXT_CLSD, clsd)                                                   \
  MACRO(RVM_ZEXT_FA, fa)                                                       \
  MACRO(RVM_ZEXT_FBFMIN, fbfmin)                                               \
  MACRO(RVM_ZEXT_FH, fh)                                                       \
  MACRO(RVM_ZEXT_FHMIN, fhmin)                                                 \
  MACRO(RVM_ZEXT_FINX, finx)                                                   \
  MACRO(RVM_ZEXT_DINX, dinx)                                                   \
  MACRO(RVM_ZEXT_HINX, hinx)                                                   \
  MACRO(RVM_ZEXT_HINXMIN, hinxmin)                                             \
  MACRO(RVM_ZEXT_BA, ba)                                                       \
  MACRO(RVM_ZEXT_BB, bb)                                                       \
  MACRO(RVM_ZEXT_BC, bc)                                                       \
  MACRO(RVM_ZEXT_BS, bs)                                                       \
  MACRO(RVM_ZEXT_BPBO, bpbo)                                                   \
  MACRO(RVM_ZEXT_BITMANIP, bitmanip)                                           \
  MACRO(RVM_ZEXT_BKB, bkb)                                                     \
  MACRO(RVM_ZEXT_BKC, bkc)                                                     \
  MACRO(RVM_ZEXT_BKX, bkx)                                                     \
  MACRO(RVM_ZEXT_PN, pn)                                                       \
  MACRO(RVM_ZEXT_PSFOPERAND, psfoperand)                                       \
  MACRO(RVM_ZEXT_VFBFMIN, vfbfmin)                                             \
  MACRO(RVM_ZEXT_VFBFWMA, vfbfwma)                                             \
  MACRO(RVM_ZEXT_VFH, vfh)                                                     \
  MACRO(RVM_ZEXT_VFHMIN, vfhmin)                                               \
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
  MACRO(RVM_ZEXT_KT, kt)                                                       \
  MACRO(RVM_ZEXT_MMUL, mmul)
#endif

typedef enum {
  RVM_FOR_EACH_ZEXT(RVM_DEFINE_ENUM_CASE) RVM_ZEXT_NUMBER
} RVMZExt;

#ifdef RVM_FOR_EACH_XEXT
#error RVM_FOR_EACH_XEXT should not be defined at this point
#else
// Only append to this list to keep ABI compatible when new extensions are
// added.
#define RVM_FOR_EACH_XEXT(MACRO)                                               \
  MACRO(RVM_XEXT_EMPTY, empty)
#endif

typedef enum {
  RVM_FOR_EACH_XEXT(RVM_DEFINE_ENUM_CASE) RVM_XEXT_NUMBER
} RVMXExt;
#undef RVM_DEFINE_ENUM_CASE

#define RVM_ZEXT_MAX 512
#define RVM_XEXT_MAX 128

#ifdef __cplusplus
static_assert(RVM_ZEXT_MAX > RVM_ZEXT_NUMBER);
static_assert(RVM_XEXT_MAX > RVM_XEXT_NUMBER);
#endif // __cplusplus

/** @brief Descriptor containing info about all extension
 *
 * Extension arrays (e.g. MisaExt) could be bitsets but this would require
 * non-trivial bitwise operations to set and check for specific extension.
 * Especially for ZExt and XExt that support addition of new extensions.
 * Considering that this bitset size is subject to change we decided to just
 * leave them as array of chars. Yes, we use 8x the amount of memory needed but
 * now adding new extensions is easy and checking for them is as easy as
 * MisaExt[RVM_MISA_M]. And this struct is usually created/copied only once per
 * program, so memory usage and performance are not an issue.
 *
 */
typedef struct RVMExtDescriptor {
  /** This field is used to check for ABI compatibility
   *
   * @attention Set this to sizeof(ZExt) on initialization
   */
  size_t ZExtSize;
  /** This field is used to check for ABI compatibility
   *
   * @attention Set this to sizeof(XExt) on initialization
   */
  size_t XExtSize;
  /** Standard single-letter extensions (e.g. "M" or "F")
   *
   * To enable extension "M" set according element to non-zero.
   *
   * For example: Exts.MisaExt[RVM_MISA_M] = 1;
   */
  char MisaExt[RVM_MISA_NUMBER];
  /** Standard multi-letter extensions (e.g. "zicsr")
   *
   * For example:
   *
   * To enable extension "Zicsr" set according element to non-zero:
   *
   * Exts.ZExt[RVM_ZEXT_] = 1;
   */
  char ZExt[RVM_ZEXT_MAX];
  /** Custom extensions
   *
   * For example:
   *
   * To enable your custom extension "xmyown" set according element to non-zero.
   *
   * For example: Exts.XExt[RVM_XEXT_MYOWN] = 1;
   */
  char XExt[RVM_XEXT_MAX];
} RVMExtDescriptor;

/** @brief Offsets of specific bit fields in XSTATUS CSR */
typedef enum {
  RVM_MSTATUS_VS_FIELD_OFFSET = 9,
  RVM_MSTATUS_FS_FIELD_OFFSET = 13,
} RVMMStatusFields;

struct RVMConfig;
typedef struct RVMConfig RVMConfig;

/** @brief Stop Mode enum */
typedef enum {
  RVM_STOP_NEVER = 0, /**< Never stop with @ref RVM_STEP_FINISH */
  RVM_STOP_BY_PC, /**< return @ref RVM_STEP_FINISH when PC reaches value set by
                 @ref * rvm_setStopPC */
} RVMStopMode;

/** @brief Execution status enum */
typedef enum RVM_NODISCARD_ENUM {
  RVM_STEP_SUCCESS,   /**<  Simulator stepped successfully, no additional event
                      happened */
  RVM_STEP_FINISH,    /**< Simulator got ebreak or instruction with "StopPC" */
  RVM_STEP_EXCEPTION, /**< Simulator got some kind of exception */
} RVMSimExecStatus;

/** @brief Error codes enum */
typedef enum RVM_NODISCARD_ENUM {
  RVM_ERRC_SUCCESS = 0,        /**< No errors occurred */
  RVM_ERRC_INVALID_ARGUMENT,   /**< One or several of the arguments were invalid
                                */
  RVM_ERRC_INVALID_ADDRESS,    /**< Memory address value is invalid */
  RVM_ERRC_VALUE_OUT_OF_RANGE, /**< Provided value is out of valid range */
  RVM_ERRC_IDX_OUT_OF_RANGE,   /**< Provided index is out of valid range */
  RVM_ERRC_INVALID_MEM_REGION, /**< Specified memory region is invalid */
  RVM_ERRC_INCOMPATIBLE,       /**< API compatibility is broken */
  RVM_ERRC_UNRECOVERABLE_ERROR, /**< Generally used when unexpected error has
                                   occurred and model is now in inconsistent
                                   state. check @ref getErrorContext to
                                   hopefully see some info */
} RVMErrorCode;

/**
 * @brief Returns a pointer to the textual description of the RVMErrorCode
 *
 * Implementation-independent
 *
 * @param Err Error code
 *
 * @returns Pointer to a null-terminated string corresponding to the `Err`.
 *
 */
const char *rvm_strerror(RVMErrorCode Err);

/**
 * @brief Returns a pointer to the textual description containing explanation on
 * why error occurred
 *
 * Message is implementation defined. All implementations are advised to insert
 * explanation that will be clear to the user. The result should always be a
 * valid C string in ASCII
 *
 * For example. If function @ref rvm_readMem returns @ref
 * RVM_ERRC_INVALID_ADDRESS error context is expected to contain the value of
 * the address that is invalid and ranges of available addresses.
 *
 * @param State Model instance
 * @param[out] Buf Pointer to the output buffer to write error context to.
 *                 Should have at least @p BufSize chars big (not including
 *                 terminating zero). If you pass NULL as this parameter
 *                 necessary size of the buffer vill be written to output
 *                 parameter BufSize.
 * @param[in,out] BufSize Maximum number of bytes available in @p Buf. If
 *                        message can't fit this buffer only BufSize chars
 *                        of the message will be copied and necessary size
 *                        will bewritten to this pointer. NULL is forbidden.
 */
void rvm_getErrorContext(const RVMState *State, char *Buf, size_t *BufSize);

/** @brief Creates model instance
 *
 * Possible Err values:
 * - @ref RVM_ERRC_SUCCESS on success.
 * - @ref RVM_ERRC_INVALID_ARGUMENT if MemoryRegions pointer is NULL
 * - @ref RVM_ERRC_INVALID_MEM_REGION if any memory regions was invalid
 * - @ref RVM_ERRC_INCOMPATIBLE if ZExtSize of XExtSize are not equal to
 * sizeof(ZExt) and sizeof(XExt) respectively
 *
 * @param config Model configuration to build with
 *
 * @param[out] Err Pointer to error code. Ignored if NULL.
 *
 * @param[out] ErrBuf Pointer to the char buffer. If was assigned anything other
 *                    than @ref RVM_ERRC_SUCCESS an appropriate error message
 * will be written to it.
 *
 * @param ErrBufSize Number of available characters in @p ErrBuf (not
 *                   including terminating zero). If actual error message cannot
 *                   fit this array it will be truncated.
 *
 * @return Created model instance pointer or NULL on error
 */
RVM_NODISCARD
RVMState *rvm_modelCreate(const RVMConfig *config, RVMErrorCode *Err,
                          char *ErrBuf, size_t ErrBufSize);

/**
 * @brief Destructs model instance
 *
 * Never fails
 *
 * @param State Model to be destroyed. NULL is allowed
 */
void rvm_modelDestroy(RVMState *State);
/**
 * @brief Resets model to the initial state
 *
 * Implementations should implement this method in a way that after reset the
 * model is in exact state as model that was just @ref rvm_modelCreate -ed
 * from the same config.
 *
 * Never fails
 *
 * @param State Model instance to reset
 */
void rvm_modelReset(RVMState *State);

/**
 * @brief Get pointer to config model was created from
 *
 * Never fails
 *
 * @param State Model instance to get config from
 * @return Pointer to the internal copy of RVMConfig passed to @ref
 * rvm_modelCreate
 */
const RVMConfig *rvm_getModelConfig(const RVMState *State);

/**
 * @brief Executes single instruction stored at the current PC
 *
 * @param State Model instance
 * @return Status of the execution. @ref RVM_STEP_SUCCESS if execution
 * succeeded without exceptions.@ref RVM_STEP_FINISH if @ref RVMStopMode stop
 * condition was met. @ref RVM_STEP_EXCEPTION if exception or interrupt
 * occurred.
 */
RVMSimExecStatus rvm_executeInstr(RVMState *State);

/**
 * @brief Reads bytes from memory
 *
 * @param State Model instance
 * @param Addr Physical address to read from
 * @param Count Number of bytes to copy
 * @param[out] Data Pointer to the memory location to copy to. Should have at
 * least Count bytes accessible
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ADDRESS if attempted to access memory that was not allocated
 * by any @ref RVMMemoryRegion
 */
RVM_NODISCARD
RVMErrorCode rvm_readMem(const RVMState *State, uint64_t Addr, size_t Count,
                         char *Data);
/**
 * @brief Writes bytes to memory
 *
 * @param State Model instance
 * @param Addr Physical address to write to
 * @param Count Number of bytes to copy
 * @param Data Pointer to the memory location to copy Count bytes from
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ADDRESS if attempted to access memory that was not allocated
 * by any @ref RVMMemoryRegion
 */
RVM_NODISCARD
RVMErrorCode rvm_writeMem(RVMState *State, uint64_t Addr, size_t Count,
                          const char *Data);

/**
 * @brief Sets model's stop mode
 *
 * @param State Model instance
 * @param Mode Stop mode
 */
void rvm_setStopMode(RVMState *State, RVMStopMode Mode);

/**
 * @brief Sets stop PC
 *
 * @param State Model instance
 * @param Addr PC value to stop execution at. Has no effect if StopMode != @ref
 * RVM_STOP_BY_PC
 *
 * @return @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ADDRESS if Addr is wider than PC register
 */
RVM_NODISCARD
RVMErrorCode rvm_setStopPC(RVMState *State, uint64_t Addr);

/**
 * @brief Reads current PC
 *
 * @param State Model instance
 * @returns Current value of PC register
 */
uint64_t rvm_readPC(const RVMState *State);
/**
 * @brief Sets PC register
 *
 * @param State Model instance
 * @param NewPC Value to write to PC register
 *
 * @return @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ADDRESS if NewPC is wider than PC register
 */
RVM_NODISCARD
RVMErrorCode rvm_setPC(RVMState *State, uint64_t NewPC);

/**
 * @brief List of GPR registers
 *
 * RVM_X_REG_0 <=> X0
 *
 * RVM_X_REG_31 <=> X31
 */
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

/**
 * @brief Reads GPR value
 *
 * Value written to Val is zero-extended to 64 bit
 *
 * @param State Model instance
 * @param Reg Register to read
 * @param[out] Val Pointer to a variable to write register value to. Untouched
 * on error.
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_readXReg(const RVMState *State, RVMXReg Reg, RVMRegT *Val);

/**
 * @brief Sets GPR to value
 *
 * @param State Model instance
 * @param Reg Register to set
 * @param Value Value that will be written to Reg
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ARGUMENT if Value is wider than register.
 * @ref RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_setXReg(RVMState *State, RVMXReg Reg, RVMRegT Value);

/**
 * @brief List of floating point registers
 *
 * RVM_F_REG_0 <=> F0
 *
 * RVM_F_REG_31 <=> F31
 */
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

/**
 * @brief Reads FPR value
 *
 * @param State Model instance
 * @param Reg Register to read
 * @param[out] Val Pointer to a variable to write register value to. Untouched
 * on error.
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_readFReg(const RVMState *State, RVMFReg Reg, RVMRegT *Val);
/**
 * @brief Sets FPR to value
 *
 * @param State Model instance
 * @param Reg Register to set
 * @param Value Value to write to register denoted by Reg
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ARGUMENT if Value is wider than register.
 * @ref RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_setFReg(RVMState *State, RVMFReg Reg, RVMRegT Value);

/**
 * @brief List of available CSR-s
 */
typedef enum {
  // RISC-V privileged spec, tables 2.2 - 2.6
  RVM_CSR_FFLAGS = 0x001, /**< FFLAGS bits from FCSR reg */
  RVM_CSR_FRM = 0x002,    /**< FRM bits from FCSR reg */
  RVM_CSR_FCSR = 0x003,   /**< Whole FCSR */
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

/**
 * @brief Reads CSR value
 *
 * @param State Model instance
 * @param Reg Register to read
 * @param[out] Val Pointer to a variable to write register value to. Untouched
 * on error.
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ADDRESS if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_readCSR(const RVMState *State, unsigned Reg, RVMRegT *Val);

/**
 * @brief Sets CSR to value
 *
 * @param State Model instance
 * @param Reg Register to set
 * @param Value Value to write to register denoted by Reg
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_VALUE_OUT_OF_RANGE if Value is wider than register.
 * @ref RVM_ERRC_INVALID_ADDRESS if unsupported Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_setCSR(RVMState *State, unsigned Reg, RVMRegT Value);

/**
 * @brief Raises external interrupt
 *
 * @param State Model instance
 * @param Value MCAUSE will be set to this value
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ARGUMENT if Value is wider than MCAUSE register.
 */
RVM_NODISCARD
RVMErrorCode rvm_raiseInterrupt(RVMState *State, RVMRegT Value);
/**
 * @brief Clears interrupt status and sets MCAUSE CSR
 *
 * @param State Model instance
 * @param Value MCAUSE will be set to this value
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_INVALID_ARGUMENT if Value is wider than MCAUSE register.
 */
RVM_NODISCARD
RVMErrorCode rvm_clearInterrupt(RVMState *State, RVMRegT Value);

/**
 * @brief List of vector registers
 *
 * RVM_V_REG_0 <=> V0
 *
 * RVM_V_REG_31 <=> V31
 */
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

/**
 * @brief Reads vector register
 *
 * @param State model instance
 * @param Reg Vector register to read
 * @param[out] Data Pointer to buffer to copy vector to
 *
 * @param[in,out] MaxSize Maximal vector register size (in bytes). Serves as a
 * limiter to avoid overflow. If @p Data is NULL then required size will be
 * written to @p MaxSize. If @p MaxSize can't fit target register only @p
 * MaxSize bits will be copied and necessary register size will be written to @p
 * MaxSize.
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported @p Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_readVReg(const RVMState *State, RVMVReg Reg, char *Data,
                          size_t *MaxSize);
/**
 * @brief Writes value to vector register
 *
 * @param State Model instance
 * @param Reg Vector register to update
 *
 * @param Data Pointer to read new vector register value from. Should have at
 * least VLENB bytes.
 *
 * @param MaxSize Maximal vector register size (in bytes). Serves as a limiter
 * to avoid overflow. If @p Data is NULL then required size will be written to
 * @p MaxSize. If @p MaxSize can't fit target register only @p MaxSize bits will
 * be copied and necessary register size will be written to @p MaxSize.
 *
 * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
 * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported @p Reg was specified
 */
RVM_NODISCARD
RVMErrorCode rvm_setVReg(RVMState *State, RVMVReg Reg, const char *Data,
                         size_t *DataSize);
/**
 * @brief appends custom message to model logs
 *
 * @param State Model instance
 * @param Message '\0'-terminated string to append
 *
 */
void rvm_logMessage(const RVMState *State, const char *Message);

/**
 * @brief Check if callbacks are implemented by the model
 *
 * @param State Model instance
 * @returns Non-zero value if callbacks are implemented
 */
RVM_NODISCARD
int rvm_queryCallbackSupportPresent(const RVMState *State);

/** @brief Pointer to a memory read callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param Addr Memory address that was read
 * @param Data Pointer to a buffer containing values that were read from memory
 * @param Size Number of bytes in Data pointer that were read from memory
 */
typedef void (*MemReadCallbackTy)(RVMCallbackHandler *, uint64_t Addr,
                                  const char *Data, size_t Size);

/** @brief Pointer to a memory update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param Addr Memory address that was written to
 * @param Data Pointer to a buffer containing values that were written to memory
 * @param Size Number of bytes in Data pointer that were written to memory
 */
typedef void (*MemUpdateCallbackTy)(RVMCallbackHandler *, uint64_t Addr,
                                    const char *Data, size_t Size);
/** @brief Pointer to a XReg update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param Reg Register that was updated
 * @param Value Value that was written to the register
 */

typedef void (*XRegUpdateCallbackTy)(RVMCallbackHandler *, RVMXReg Reg,
                                     RVMRegT Value);
/** @brief Pointer to a FReg update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param Reg Register that was updated
 * @param Value Value that was written to the register
 */
typedef void (*FRegUpdateCallbackTy)(RVMCallbackHandler *, RVMFReg Reg,
                                     RVMRegT Value);
/** @brief Pointer to a VReg update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param Reg Register that was updated
 * @param Data Pointer to array of bytes that were written to the register
 * @param Size Number of bytes in register
 */
typedef void (*VRegUpdateCallbackTy)(RVMCallbackHandler *, RVMVReg Reg,
                                     const char *Data, size_t Size);
/** @brief Pointer to a CSR update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param CSR Register that was updated
 * @param Value Value that was written to the CSR
 */
typedef void (*CSRUpdateCallbackTy)(RVMCallbackHandler *, RVMCSR CSR,
                                    uint64_t Value);
/** @brief Pointer to a PC update callback function
 *
 * @param CallbackHandler Pointer to a handler
 * @param PC New value of the PC register
 */
typedef void (*PCUpdateCallbackTy)(RVMCallbackHandler *, uint64_t PC);

/** @brief Memory region descriptor
 *
 * This class is used to denote a contiguous memory region the model should
 * allocate.
 *
 * The inclusive range is [Start, Start + Size - 1]. I.e. Size bytes
 * starting from address Start
 */
struct RVMMemoryRegion {
  /** @brief Address of the first byte in the allocated region */
  uint64_t Start;
  /** @brief Size of the memory region */
  uint64_t Size;
  /** @brief '\0'-terminated name of the region.
   *
   * This field is used only for debugging and error reporting.
   *
   * Set to NULL to ignore
   */
  const char *Name;
};

/** @brief Full configuration for the model(implementation) */
struct RVMConfig {
  /** @brief Array of memory regions that model should make accessible
   *
   * MemoryRegions shall be copied by implementation on @ref rvm_modelCreate.
   * This can be both pointer to local variable and dynamically allocated array.
   * The only requirement for users is that this array should live at least up
   * to
   * @ref rvm_modelCreate call.
   *
   * \important Implementation shall merge all intersecting Memory regions. That
   * is if given [0, 100] and [50, 150] model should treat them as one region
   * [0, 150]. Implementation may name this new merged region as it sees fit.
   *
   */
  const struct RVMMemoryRegion *MemoryRegions;
  /** @brief Number of elements in @ref MemoryRegions */
  unsigned MemoryRegionCount;
  /** @brief Boolean value that specifies if we are modeling rv32 or rv64
   * processor. Any non-zero value means rv64
   */
  int RV64;
  /** @brief VLEN used by vector extensions (if any) */
  unsigned VLEN;
  /** @brief Whether or not to trap on misaligned access (essentially the same
   * as Zicclsm and should be deleted when we add it to the
   * @ref RVMZExt enum)
   */
  int EnableMisalignedAccess;
  /** @brief StopMode to be used by implementation
   *
   *  @attention Better just use @ref rvm_setStopMode
   *
   */
  RVMStopMode Mode;
  /** @brief Address to stop execution at if @ref Mode is set to @ref
   * RVM_STOP_BY_PC
   *
   * @attention Better just use @ref rvm_setStopPC
   *
   */
  uint64_t StopAddr;
  /** @brief Path to a file to save model logs to
   *
   * NULL to disable logs, empty
   * string - stdout, "-" for stderr.
   */
  const char *LogFilePath;
  /** @brief Path to a file to save debug logs to
   *
   * NULL to disable logs, empty
   * string - stdout, "-" for stderr.
   */
  const char *DebugLogFilePath;
  /** @brief CallbackHandler pointer that model will pass to every later
   * callbacks as a parameter
   *
   * @important No callbacks will be called if this member is set to NULL
   *
   */
  RVMCallbackHandler *CallbackHandler;
  /** @brief Callback function that model will call each time executed
   * instruction reads from memory
   */
  MemReadCallbackTy MemReadCallback;
  /** @brief Callback function that model will call each time executed
   * instruction writes to memory
   */
  MemUpdateCallbackTy MemUpdateCallback;
  /** @brief Callback function that model will call each time executed
   * instruction writes to X register
   */
  XRegUpdateCallbackTy XRegUpdateCallback;
  /** @brief Callback function that model will call each time executed
   * instruction writes to F register
   */
  FRegUpdateCallbackTy FRegUpdateCallback;
  /** @brief Callback function that model will call each time executed
   * instruction writes to V register
   */
  VRegUpdateCallbackTy VRegUpdateCallback;
  /** @brief Callback function that model will call each time executed
   * instruction writes to CSR
   */
  CSRUpdateCallbackTy CSRUpdateCallback;
  /** @brief Callback function that model will call each time PC gets updated */
  PCUpdateCallbackTy PCUpdateCallback;
  /** @brief Whether or not to set mask agnostic bits in vector operations (if
   * present)
   */
  int ChangeMaskAgnosticElems;
  /** @brief Whether or not to set tail agnostic bits in vector operations (if
   * present)
   */
  int ChangeTailAgnosticElems;
  /** @brief ISA Extension info */
  RVMExtDescriptor Extensions;
};

/** @brief Typedef for @ref rvm_modelCreate type */
typedef RVMState *(*rvm_modelCreate_t)(const RVMConfig *config,
                                       RVMErrorCode *Err, char *ErrBuf,
                                       size_t ErrBufSize);
/** @brief Typedef for @ref rvm_modelDestroy type */
typedef void (*rvm_modelDestroy_t)(RVMState *);
/** @brief Typedef for @ref rvm_modelReset type */
typedef void (*rvm_modelReset_t)(RVMState *);
/** @brief Typedef for @ref rvm_getModelConfig type */
typedef const RVMConfig *(*rvm_getModelConfig_t)(const RVMState *);
/** @brief Typedef for @ref rvm_executeInstr type */
typedef RVMSimExecStatus (*rvm_executeInstr_t)(RVMState *);
/** @brief Typedef for @ref rvm_readMem type */
typedef RVMErrorCode (*rvm_readMem_t)(const RVMState *, uint64_t, size_t,
                                      char *);
/** @brief Typedef for @ref rvm_writeMem type */
typedef RVMErrorCode (*rvm_writeMem_t)(RVMState *, uint64_t, size_t,
                                       const char *);
/** @brief Typedef for @ref rvm_readPC type */
typedef uint64_t (*rvm_readPC_t)(const RVMState *);
/** @brief Typedef for @ref rvm_setPC type */
typedef RVMErrorCode (*rvm_setPC_t)(RVMState *, uint64_t);
/** @brief Typedef for @ref rvm_readXReg type */
typedef RVMErrorCode (*rvm_readXReg_t)(const RVMState *, RVMXReg, RVMRegT *);
/** @brief Typedef for @ref rvm_setXReg type */
typedef RVMErrorCode (*rvm_setXReg_t)(RVMState *, RVMXReg, RVMRegT);
/** @brief Typedef for @ref rvm_readFReg type */
typedef RVMErrorCode (*rvm_readFReg_t)(const RVMState *, RVMFReg, RVMRegT *);
/** @brief Typedef for @ref rvm_setFReg type */
typedef RVMErrorCode (*rvm_setFReg_t)(RVMState *, RVMFReg, RVMRegT);
/** @brief Typedef for @ref rvm_setStopMode type */
typedef void (*rvm_setStopMode_t)(RVMState *, RVMStopMode);
/** @brief Typedef for @ref rvm_setStopPC type */
typedef RVMErrorCode (*rvm_setStopPC_t)(RVMState *, uint64_t);
/** @brief Typedef for @ref rvm_readCSR type */
typedef RVMErrorCode (*rvm_readCSR_t)(const RVMState *, unsigned, RVMRegT *);
/** @brief Typedef for @ref rvm_setCSR type */
typedef RVMErrorCode (*rvm_setCSR_t)(RVMState *, unsigned, RVMRegT);
/** @brief Typedef for @ref rvm_readVReg type */
typedef RVMErrorCode (*rvm_readVReg_t)(const RVMState *, RVMVReg, char *,
                                       size_t *);
/** @brief Typedef for @ref rvm_setVReg type */
typedef RVMErrorCode (*rvm_setVReg_t)(RVMState *, RVMVReg, const char *,
                                      size_t *);
/** @brief Typedef for @ref rvm_logMessage type */
typedef void (*rvm_logMessage_t)(const RVMState *, const char *);
/** @brief Typedef for @ref rvm_queryCallbackSupportPresent type */
typedef int (*rvm_queryCallbackSupportPresent_t)(const RVMState *);
/** @brief Typedef for @ref rvm_raiseInterrupt type */
typedef RVMErrorCode (*rvm_raiseInterrupt_t)(RVMState *, RVMRegT);
/** @brief Typedef for @ref rvm_clearInterrupt type */
typedef RVMErrorCode (*rvm_clearInterrupt_t)(RVMState *, RVMRegT);
/** @brief Typedef for @ref rvm_getErrorContext type */
typedef void (*rvm_getErrorContext_t)(const RVMState *, char *, size_t *);

#ifdef __cplusplus
}
#endif // __cplusplus

