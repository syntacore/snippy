//===-- DynLibLoader.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_LIB_DYNLIBLOADER_H
#define LLVM_TOOLS_SNIPPY_LIB_DYNLIBLOADER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace llvm {

namespace snippy {

using NameModifier = std::function<std::string(StringRef)>;

class DynamicLibrary {
public:
  DynamicLibrary(
      StringRef LibraryPath,
      std::optional<NameModifier> LibPathModif = std::optional<NameModifier>{});
  void *getAddressOfSymbol(const char *symbolName) const;

private:
  void *Handle;
};

std::string makeAltPathForDynLib(StringRef Name);
std::optional<std::string> findLibraryPath(StringRef BaseLibName);
std::string getDynLibPath(
    StringRef PluginLib,
    std::optional<NameModifier> LibPathModif = std::optional<NameModifier>{});
} // namespace snippy

} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_DYNLIBLOADER_H
