//===-- VTable.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "RVM.h"

/** @file VTable.h This file contains definition for VTable used to dispatch
 * between C API functions from @ref RVM.h */

#ifdef __cplusplus
extern "C" {
/** @brief Contains VTable definition */
namespace rvm {
#endif // __cplusplus

/** @brief VTable used to dispatch to RVM interface implementation
 *
 * These function pointers are assigned by RVM implementation and read by C++
 * API in @ref RVM.hpp on user's side
 */
struct RVM_FunctionPointers {
  rvm_modelCreate_t modelCreate;
  rvm_modelDestroy_t modelDestroy;
  rvm_modelReset_t modelReset;

  rvm_getModelConfig_t getModelConfig;

  rvm_executeInstr_t executeInstr;

  rvm_readMem_t readMem;
  rvm_writeMem_t writeMem;

  rvm_setStopMode_t setStopMode;
  rvm_setStopPC_t setStopPC;

  rvm_readPC_t readPC;
  rvm_setPC_t setPC;

  rvm_readXReg_t readXReg;
  rvm_setXReg_t setXReg;

  rvm_readFReg_t readFReg;
  rvm_setFReg_t setFReg;

  rvm_readCSR_t readCSR;
  rvm_setCSR_t setCSR;

  rvm_readVReg_t readVReg;
  rvm_setVReg_t setVReg;

  rvm_raiseInterrupt_t raiseInterrupt;
  rvm_clearInterrupt_t clearInterrupt;

  rvm_logMessage_t logMessage;
  rvm_queryCallbackSupportPresent_t queryCallbackSupportPresent;
  rvm_getErrorContext_t getErrorContext;
};

#ifdef __cplusplus
}
} // namespace rvm
#endif // __cplusplus

