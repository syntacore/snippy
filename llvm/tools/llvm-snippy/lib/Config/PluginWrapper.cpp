//===-- PluginWrapper.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/PluginWrapper.h"
#include "snippy/Support/DynLibLoader.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <memory>

#include "snippy/Support/OpcodeCache.h"

namespace llvm {
namespace snippy {

void PluginManager::loadPluginDL(const std::string &PluginLibName) {
  auto Lib = DynamicLibrary(PluginLibName);
  const auto *VTable = reinterpret_cast<const PluginFunctionsTable *>(
      Lib.getAddressOfSymbol(PLUGIN_ENTRY_NAME));
  if (!VTable)
    report_fatal_error("Can't find entry point of plugin generator", false);
  DLTable = VTable;
}

unsigned getOpcodeFromStr(const char *Str,
                          const OpcodeCacheHandle *OpcCacheHandle) {
  if (!OpcCacheHandle)
    report_fatal_error("Null opcode cache handle", false);
  auto OpcCache = reinterpret_cast<const OpcodeCache *>(OpcCacheHandle);
  auto Opcode = OpcCache->code(Str);
  if (!Opcode)
    report_fatal_error("Incorrect opcode in plugin: [" + Twine(Str) + "]",
                       false);
  return Opcode.value();
}

struct Deleter {
  void operator()(void *Ptr) { ::operator delete(Ptr); }
};

using AllocatedMem = std::unique_ptr<void, Deleter>;

class PluginMemoryManager final {
  AllocatedMem Memory;

public:
  void *allocateMemory(unsigned Size) {
    Memory.reset(::operator new(Size));
    return Memory.get();
  }
};

// this function provides TEMPORARY memory for plugin message
// the message lifetime ends when a new one is allocated
void *allocateMemory(unsigned Size) {
  static PluginMemoryManager MemManager;
  return MemManager.allocateMemory(Size);
}

} // namespace snippy
} // namespace llvm
