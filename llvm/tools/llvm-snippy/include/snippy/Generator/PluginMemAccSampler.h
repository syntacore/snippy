//===-- PluuginMemAccSampler.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_GENERATOR_PLUGIN_MEM_ACCESS_SAMPLER_H
#define LLVM_SNIPPY_INCLUDE_GENERATOR_PLUGIN_MEM_ACCESS_SAMPLER_H

#include "snippy/Config/MemorySchemePluginWrapper.h"
#include "snippy/Generator/MemAccessSampler.h"

#include <filesystem>

namespace llvm {
namespace snippy {
class PluginMemoryAccessSampler final : public IMemoryAccessSampler {
  MemorySchemePluginWrapper Plugin;
  std::filesystem::path OriginalFile;

public:
  PluginMemoryAccessSampler(MemorySchemePluginWrapper P
                            )
      : Plugin(std::move(P))
  {
  }

  Expected<AccessSampleResult>
  sample(size_t AccessSize, size_t Alignment,
         std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,
         std::optional<::AddressGlobalId> PreselectedId = std::nullopt,
         bool BurstMode = false) override;

  virtual std::optional<::AddressGlobalId>
  getPreselectedAddressId() const override {
    if (!Plugin.isEnabled())
      return std::nullopt;
    return Plugin.getAddressId();
  }

  void reserve(MemRange) override {}

  void setAddrPlugin(MemorySchemePluginWrapper Plug) {
    Plugin = std::move(Plug);
  }

  auto &getFilename() const { return OriginalFile; }

  std::string getName() const override {
    return "plugin memory access sampler";
  }

  void print(raw_ostream &OS) const override;
};
} // namespace snippy
} // namespace llvm
#endif
