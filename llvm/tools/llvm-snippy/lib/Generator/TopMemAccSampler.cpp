//===-- TopMemAccSampler.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/TopMemAccSampler.h"
#include "snippy/Generator/GeneratorContext.h"

namespace llvm {
namespace snippy {

Error TopLevelMemoryAccessSampler::reportSamplersError(
    const SmallVectorImpl<std::string> &Errs) {
  std::string ErrMsg;
  raw_string_ostream OS(ErrMsg);
  for (auto &&[Idx, E] : enumerate(Errs))
    OS << Idx << ") " << Samplers[Idx]->getName() << ": " << E << "\n";
  return make_error<MemoryAccessSampleError>(
      Twine("All samplers failed to generate memory access:\n")
          .concat(StringRef(ErrMsg).rtrim()));
}

Expected<AddressInfo>
TopLevelMemoryAccessSampler::sample(const AddressGenInfo &AddrGenInfo) {
  SmallVector<std::string, 3> Errs(Samplers.size());
  for (auto &&[Idx, S] : enumerate(Samplers)) {
    auto Access = S->sample(AddrGenInfo);
    if (!Access) {
      Errs[Idx] = toString(Access.takeError());
      continue;
    }
    return *Access;
  }
  return reportSamplersError(Errs);
}

Expected<MemoryAccess &>
TopLevelMemoryAccessSampler::chooseAccess(const AddressGenInfo &AddrGenInfo) {
  SmallVector<std::string, 3> Errs(Samplers.size());
  for (auto &&[Idx, S] : enumerate(Samplers)) {
    auto Access = S->chooseAccess(AddrGenInfo);
    if (!Access) {
      Errs[Idx] = toString(Access.takeError());
      continue;
    }
    return *Access;
  }
  return reportSamplersError(Errs);
}

std::vector<AddressInfo> TopLevelMemoryAccessSampler::randomBurstGroupAddresses(
    ArrayRef<AddressRestriction> ARRange, const OpcodeCache &OpcC,
    const SnippyTarget &SnpTgt) {
  assert(!ARRange.empty());

  std::vector<AddressInfo> Addresses;
  for (auto &AR : ARRange) {

    AddressGenInfo AddrGenInfo{AR.AccessSize, AR.AccessAlignment,
                               AR.AllowMisalign, /*Burst=*/true};
    auto Access = sample(AddrGenInfo);
    if (!Access)
      snippy::fatal("Failed to sample memory access for burst group",
                    toString(Access.takeError()));
    Addresses.push_back(std::move(*Access));
  }

  return Addresses;
}

} // namespace snippy
} // namespace llvm
