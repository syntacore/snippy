//===-- MemorySchemePluginWrapper.cpp
//-------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/MemorySchemePluginWrapper.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Support/DynLibLoader.h"

namespace llvm {
namespace snippy {

void MemorySchemePluginWrapper::loadPluginDL(StringRef PluginLibName) {
  auto Lib = DynamicLibrary(PluginLibName);
  const auto *VTable =
      reinterpret_cast<const ::MemorySchemePluginFunctionsTable *>(
          Lib.getAddressOfSymbol(MEMORY_SCHEME_PLUGIN_ENTRY_NAME));
  if (!VTable)
    snippy::fatal("Can't find entry point of memory scheme plugin");
  DLTable = VTable;
}

void MemorySchemePluginWrapper::setMemSchemePluginInfo(
    StringRef MemSchemePluginInfoFile) const {
  assert(DLTable && "Plugin hasn't been loaded yet");
  if (DLTable->setAddressInfoFile == nullptr)
    snippy::fatal("Invalid memory scheme plugin functions table:"
                  " missing setAddressInfoFile()");
  DLTable->setAddressInfoFile(MemSchemePluginInfoFile.str().c_str());
}

std::optional<AddressInfo>
MemorySchemePluginWrapper::getAddress(size_t AccessSize, size_t Alignment,
                                      bool BurstMode,
                                      size_t InstrClassId) const {
  assert(DLTable && "Plugin hasn't been loaded yet");
  if (DLTable->generateAddress == nullptr)
    snippy::fatal("Invalid memory scheme plugin functions table:"
                  " missing generateAddress()");
  auto GenMode = ::GenModeT{};
  auto Address = DLTable->generateAddress(AccessSize, Alignment, BurstMode,
                                          InstrClassId, &GenMode);
  if (GenMode != CANNOT_GENERATE_ADDRESS && GenMode != CAN_GENERATE_ADDRESS)
    snippy::fatal("Bad response from memory scheme plugin: "
                  "invalid generation mode");
  if (GenMode == CANNOT_GENERATE_ADDRESS)
    return std::nullopt;

  auto FinalAddressInfo = AddressInfo{};
  FinalAddressInfo.Address = Address;
  FinalAddressInfo.AccessSize = AccessSize;
  return FinalAddressInfo;
}

::AddressGlobalId MemorySchemePluginWrapper::getAddressId() const {
  assert(DLTable && "Plugin hasn't been loaded yet");
  if (DLTable->generateAddressId == nullptr)
    snippy::fatal("Invalid memory scheme plugin functions table:"
                  " missing generateAddressId()");
  auto AddrId = ::AddressGlobalId{0, {0, 0}};
  DLTable->generateAddressId(&AddrId);
  return AddrId;
}

} // namespace snippy
} // namespace llvm
