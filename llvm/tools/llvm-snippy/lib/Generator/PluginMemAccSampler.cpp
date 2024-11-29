//===-- PluginMemAccSampler.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/PluginMemAccSampler.h"
namespace llvm {
namespace snippy {
Expected<AccessSampleResult> PluginMemoryAccessSampler::sample(
    size_t AccessSize, size_t Alignment,
    std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,
    std::optional<::AddressGlobalId> Preselected, bool BurstMode) {
  if (!Plugin.isEnabled())
    return make_error<MemoryAccessSampleError>("Plugin is disabled");
  auto Addr =
      transformOpt(Plugin.getAddress(AccessSize, Alignment, BurstMode,
              0 /* placeholder */
                                         ),
                   [&](auto AI) {
                     return AccessSampleResult{
                         AI, AddressGenInfo{AccessSize, Alignment, BurstMode}};
                   });
  if (!Addr.has_value()) {
    return make_error<MemoryAccessSampleError>(formatv(
        "Plugin failed to sample address for: size: {0}, alignment: {1}",
        AccessSize, Alignment));
  }
  return *Addr;
}

void PluginMemoryAccessSampler::print(raw_ostream &OS) const {
  OS << getName() << ":\n";
  OS << "OriginalFile: " << OriginalFile << "\n";
}
} // namespace snippy
} // namespace llvm
