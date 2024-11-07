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

Expected<AccessSampleResult> TopLevelMemoryAccessSampler::sample(
    size_t AccessSize, size_t Alignment,
    std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,
    std::optional<::AddressGlobalId> Preselected, bool BurstMode) {
  SmallVector<std::string, 3> Errs(Samplers.size());
  for (auto &&[Idx, S] : enumerate(Samplers)) {
    auto Access = S->sample(AccessSize, Alignment, ChooseAddrGenInfo,
                            Preselected, BurstMode);
    if (auto Err = Access.takeError()) {
      Preselected = S->getPreselectedAddressId();
      Errs[Idx] = toString(std::move(Err));
      continue;
    }
    return *Access;
  }
  std::string ErrMsg;
  raw_string_ostream OS(ErrMsg);
  for (auto &&[Idx, E] : enumerate(Errs))
    OS << Idx << ") " << Samplers[Idx]->getName() << ": " << E << "\n";
  return make_error<MemoryAccessSampleError>(
      Twine("All samplers failed to generate memory access:\n")
          .concat(StringRef(ErrMsg).rtrim()));
}

std::vector<AddressInfo> TopLevelMemoryAccessSampler::randomBurstGroupAddresses(
    ArrayRef<AddressRestriction> ARRange, const OpcodeCache &OpcC,
    const SnippyTarget &SnpTgt) {
  assert(!ARRange.empty());

  std::vector<AddressInfo> Addresses;
  for (auto &AR : ARRange) {

    auto Access = sample(AR.AccessSize, AR.AccessAlignment,
                         /*BurstMode*/ true);
    if (auto Err = Access.takeError())
      snippy::fatal("Failed to sample memory access for burst group",
                    toString(std::move(Err)));
    auto &AI = Access->AddrInfo;
    Addresses.push_back(std::move(AI));
  }

  return Addresses;
}

} // namespace snippy
} // namespace llvm
