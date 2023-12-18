//===-- VTable.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "RVM.h"

#ifdef __cplusplus
extern "C" {
namespace rvm {
#endif // __cplusplus

struct RVM_FunctionPointers {
  rvm_modelCreate_t modelCreate;
  rvm_modelDestroy_t modelDestroy;

  rvm_getModelConfig_t getModelConfig;

  rvm_executeInstr_t executeInstr;

  rvm_readMem_t readMem;
  rvm_writeMem_t writeMem;

  rvm_readPC_t readPC;
  rvm_setPC_t setPC;

  rvm_readXReg_t readXReg;
  rvm_setXReg_t setXReg;

  rvm_readFReg_t readFReg;
  rvm_setFReg_t setFReg;

  rvm_readCSRReg_t readCSRReg;
  rvm_setCSRReg_t setCSRReg;

  rvm_readVReg_t readVReg;
  rvm_setVReg_t setVReg;

  rvm_logMessage_t logMessage;
  rvm_queryCallbackSupportPresent_t queryCallbackSupportPresent;
};

#ifdef __cplusplus
}
} // namespace rvm
#endif // __cplusplus
