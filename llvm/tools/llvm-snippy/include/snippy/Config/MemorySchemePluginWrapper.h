//===-- MemorySchemePluginWrapper.h -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"

#include "snippy/Plugins/MemorySchemePluginCInterface.h"

#include <optional>

namespace llvm {
namespace snippy {

struct AddressInfo;
struct AddressId;

class MemorySchemePluginWrapper final {
  const ::MemorySchemePluginFunctionsTable *DLTable = nullptr;

  void loadPluginDL(StringRef PluginLibName);
  void setMemSchemePluginInfo(StringRef MemSchemePluginInfoFile) const;

public:
  MemorySchemePluginWrapper(StringRef PluginLibName = "",
                            StringRef MemSchemePluginInfoFile = "") {
    if (!PluginLibName.empty())
      loadPluginDL(PluginLibName);
    if (!MemSchemePluginInfoFile.empty())
      setMemSchemePluginInfo(MemSchemePluginInfoFile);
  }

  bool isEnabled() const { return DLTable != nullptr; }

  std::optional<AddressInfo> getAddress(size_t AccessSize, size_t Alignment,
                                        bool BurstMode,
                                        size_t InstrClassId) const;

  ::AddressGlobalId getAddressId() const;
};

} // namespace snippy
} // namespace llvm
