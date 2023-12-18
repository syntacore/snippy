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
    report_fatal_error("Can't find entry point of memory scheme plugin", false);
  DLTable = VTable;
}

void MemorySchemePluginWrapper::setMemSchemePluginInfo(
    StringRef MemSchemePluginInfoFile) const {
  assert(DLTable && "Plugin hasn't been loaded yet");
  if (DLTable->setAddressInfoFile == nullptr)
    report_fatal_error("Invalid memory scheme plugin functions table:"
                       " missing setAddressInfoFile()",
                       false);
  DLTable->setAddressInfoFile(MemSchemePluginInfoFile.str().c_str());
}

std::optional<AddressInfo>
MemorySchemePluginWrapper::getAddress(size_t AccessSize, size_t Alignment,
                                      bool BurstMode,
                                      size_t InstrClassId) const {
  assert(DLTable && "Plugin hasn't been loaded yet");
  if (DLTable->generateAddress == nullptr)
    report_fatal_error("Invalid memory scheme plugin functions table:"
                       " missing generateAddress()",
                       false);
  auto GenMode = ::GenModeT{};
  auto Address = DLTable->generateAddress(AccessSize, Alignment, BurstMode,
                                          InstrClassId, &GenMode);
  if (GenMode != CANNOT_GENERATE_ADDRESS && GenMode != CAN_GENERATE_ADDRESS)
    report_fatal_error("Bad response from memory scheme plugin: "
                       "invalid generation mode",
                       false);
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
    report_fatal_error("Invalid memory scheme plugin functions table:"
                       " missing generateAddressId()",
                       false);
  auto AddrId = ::AddressGlobalId{0u, 0u, 0u};
  DLTable->generateAddressId(&AddrId);
  return AddrId;
}

} // namespace snippy
} // namespace llvm