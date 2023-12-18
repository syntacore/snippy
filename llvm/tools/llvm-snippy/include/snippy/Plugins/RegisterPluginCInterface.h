//===-- RegisterPluginCInterface.h --------------------------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Interface for custom generation of registers
///
//===----------------------------------------------------------------------===//

#ifndef REGISTER_PLUGIN_C_INTERFACE_H_
#define REGISTER_PLUGIN_C_INTERFACE_H_

#define REG_PLUGIN_ENTRY_NAME "RegFuncTable"

#ifdef __cplusplus
extern "C" {
#endif

struct RegVerifierHandle;
struct RegTranslatorHandle;

enum RegValidation { REG_IS_INVALID = 0, REG_IS_VALID = 1 };

// Generation session - period between 2 setRegContext() calls
// Each handle is valid only during current generation session,
//  but it may change behavior from call to call in the same session.
// Each handle represents state of the current operand,
//  not the current instruction.
struct SnippyRegContext {
  // Returns REG_IS_VALID if RegIdx corresponds to the valid register
  //  in the current RegClass
  const RegVerifierHandle *RegVerifierHandleObj;
  enum RegValidation (*regIsValid)(
      unsigned RegIdx, const RegVerifierHandle *RegVerifierHandleObj);
  // Translates str to register index.
  // If RegStr is a register from the current RegClass
  //  then translator returns RegIdx: regIsValid(RegIdx) == REG_IS_VALID
  // If RegStr is not a register then regIsValid(RegIdx) == REG_IS_INVALID
  const RegTranslatorHandle *RegTranslatorHandleObj;
  unsigned (*getRegFromStr)(const char *RegStr,
                            const RegTranslatorHandle *RegTranslatorHandleObj);
};

enum RegGenResponse { REQUEST_IS_INVALID = 0, REQUEST_IS_VALID = 1 };

struct RegPluginResponse {
  enum RegGenResponse GenResult;
  unsigned RegisterIdx;
};

// called from snippy

// Prologue of reg generation
//  Starts new generation session.
void setSnippyRegContext(struct SnippyRegContext);
// Generates a register corresponds to RegVerifier from SnippyRegContext
// Potentially valid registers have indexes from 0 to MaxRegIndex
struct RegPluginResponse snippyPluginGenReg(unsigned MaxRegIndex);
// If reg-plugin-info-file flag is specified
//  snippy passes RegFileName to reg plugin
void snippyPluginSetRegInfo(const char *RegFileName);

// Plugin-snippy register agreement process:
//  Reg-plugin should work within snippy registers limits.
//  These limits may change during operands generaion for one instruction,
//  so reg-plugin has the opportunity to reject registers
//  by sending RegPluginResponse with REQUEST_IS_INVALID flag.
//  In this case snippy rolls back all the register that has been generated
//  since last setSnippyRegContext() funstion call
//  and sends new constraints with setSnippyRegContext().
//  In case of successful generation of operands of the current instruction
//  snippy sends next instruction register constraints.

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
// table for linking with snippy
struct RegPluginFunctionsTable {
  void (*setContext)(struct SnippyRegContext);
  struct RegPluginResponse (*generate)(unsigned MaxRegIndex);
  void (*setRegInfoFile)(const char *RegFileName);
};
#ifdef __cplusplus
}
#endif

#endif // REGISTER_PLUGIN_C_INTERFACE_H_