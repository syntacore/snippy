//===-- PluginCInterface.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#define PLUGIN_ENTRY_NAME "FuncTable"

#ifdef __cplusplus
extern "C" {
#endif

struct OpcodeCacheHandle;

// this structure represents context that Snippy provides to the plugin
struct SnippyContext {
  const OpcodeCacheHandle *OpcCacheHandleObj;

  void *(*allocateMemory)(unsigned Size);
  unsigned (*getOpcodeFromStr)(const char *,
                               const OpcodeCacheHandle *OpcCacheHandle);
};

struct Opcodes {
  long unsigned Num;
  const unsigned *Data;
};

typedef int OpcodeGroupIDTy;

enum OkToParse {
  PARSING_NOT_SUPPORTED = 0,
  PARSING_SUPPORTED = 1,
};

// called from snippy
void setSnippyContext(struct SnippyContext);
// plugin returns opcode from the set corresponds to OpcodeGroupID
unsigned snippyPluginGenerate(OpcodeGroupIDTy OpcodeGroupID);
// plugin receives opcodes and returns ID of the set of this opcodes
OpcodeGroupIDTy snippyPluginSendOpcodes(struct Opcodes Opc);
// plugin fills Opcodes struct with opcodes from Filename or returns
// PARSING_NOT_SUPPORTED otherwise
enum OkToParse snippyPluginParseOpcodes(struct Opcodes *pOpc,
                                        const char *FileName);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// table for linking with Snippy
struct PluginFunctionsTable {
  void (*setContext)(struct SnippyContext);
  unsigned (*generate)(OpcodeGroupIDTy OpcodeGroupID);
  OpcodeGroupIDTy (*sendOpcodes)(struct Opcodes Opc);
  enum OkToParse (*parseOpcodes)(struct Opcodes *pOpc, const char *FileName);
};

#ifdef __cplusplus
}
#endif
