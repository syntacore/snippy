//===-- DynLibLoader.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FormatVariadic.h"

#include <dlfcn.h>

namespace llvm {
namespace snippy {

namespace {

void *getPermanentLibrary(const char *Filename, std::string *errMsg = nullptr) {
  void *Handle = ::dlopen(Filename, RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
  if (!Handle) {
    if (errMsg)
      *errMsg = ::dlerror();
  }

  return Handle;
}

} // namespace

std::string makeAltPathForDynLib(StringRef Name) {
  static int Dummy = 0;
  const char *ptr = nullptr;
  auto MainExec = sys::fs::getMainExecutable(ptr, &Dummy);
  auto ParentDir = sys::path::parent_path(MainExec);
  SmallString<128> LibPath(ParentDir);
  sys::path::append(LibPath, Name);
  return LibPath.str().str();
}

std::optional<std::string> findLibraryPath(StringRef BaseLibName) {
  if (sys::path::has_parent_path(BaseLibName)) {
    DEBUG_WITH_TYPE("plugins", dbgs() << "Gived some path: <" << BaseLibName
                                      << ">, no need to check others\n");
    return BaseLibName.str();
  }
  // Check near executable.
  auto NearExecLibFile = makeAltPathForDynLib(BaseLibName);
  DEBUG_WITH_TYPE("plugins", dbgs() << "Try to find <" << BaseLibName
                                    << "> near executable, with path <"
                                    << NearExecLibFile << ">\n");
  if (sys::fs::exists(NearExecLibFile))
    return NearExecLibFile;
  DEBUG_WITH_TYPE("plugins",
                  dbgs() << "BaseLibName <" << BaseLibName << "> not found\n");
  return {};
}

std::string getDynLibPath(StringRef DynLib,
                          std::optional<NameModifier> LibPathModif) {
  auto FindRawName = findLibraryPath(DynLib);
  if (FindRawName) {
    DEBUG_WITH_TYPE("plugins", dbgs() << "Found library candidate "
                                      << FindRawName.value() << "\n");
    return FindRawName.value();
  }
  std::optional<std::string> FindModifiedName;
  if (LibPathModif &&
      (FindModifiedName = findLibraryPath(LibPathModif.value()(DynLib)))) {
    DEBUG_WITH_TYPE("plugins", dbgs() << "Found library candidate "
                                      << FindModifiedName.value() << "\n");
    return FindModifiedName.value();
  }
  snippy::fatal(formatv("could not find library for plugin: {0}", DynLib));
}

DynamicLibrary::DynamicLibrary(StringRef LibraryPath,
                               std::optional<NameModifier> LibPathModif) {
  auto DynLibraryPath = getDynLibPath(LibraryPath, LibPathModif);
  DEBUG_WITH_TYPE("plugins", dbgs() << "Trying to load dynamic library at <"
                                    << DynLibraryPath << ">\n");
  std::string ErrMsg;
  Handle = getPermanentLibrary(DynLibraryPath.c_str(), &ErrMsg);
  if (Handle)
    return;

  llvm::snippy::fatal(formatv("could not load library: {0}", ErrMsg));
}

void *DynamicLibrary::getAddressOfSymbol(const char *symbolName) const {
  auto *Sym = ::dlsym(Handle, symbolName);
  if (!Sym) {
    snippy::fatal(
        formatv("Failed to fetch symbol {0}: {1}", symbolName, ::dlerror));
  }
  return Sym;
}

} // namespace snippy
} // namespace llvm
