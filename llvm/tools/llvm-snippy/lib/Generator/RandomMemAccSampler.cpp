//===-- RandomMemAccSampler.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/RandomMemAccSampler.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {
namespace snippy {



void RandomMemoryAccessSampler::add(std::unique_ptr<MemoryAccess> Acc) {
  auto Allowed = Acc->getPossibleAddresses();
  RestrictedMB = RestrictedMB.diff(Allowed);
  BaseAccesses.emplace_back(std::move(Acc));
  updateMemoryBank();
}

void RandomMemoryAccessSampler::print(raw_ostream &OS) const {
  OS << getName() << ":\n";
  auto SetWriteDefault = [](auto &IO) { IO.setWriteDefaultValues(true); };
  MemoryScheme MAcc;
  llvm::transform(SplitAccesses, std::back_inserter(MAcc.BaseAccesses),
                  [](auto &A) { return A->copy(); });
  outputYAMLToStream(MAcc, OS, SetWriteDefault);
}

void RandomMemoryAccessSampler::updateMAG() {
  static auto MagCount = 0ull;
  MagCount++;
  std::vector<MemoryAccess *> Schemes;
  for (auto &Scheme : make_range(SplitAccesses.begin(), SplitAccesses.end()))
    Schemes.emplace_back(std::addressof(*Scheme));
  MAG = MemAccGenerator{std::move(Schemes), MagCount};
}

MemoryAccessesGenerator &RandomMemoryAccessSampler::getMAG() {
  return MAG;
}

void RandomMemoryAccessSampler::reserve(MemRange R) {
  RestrictedMB.addRange(MemRange{R.Start, R.End});
  MB = MB.diff(RestrictedMB);
  updateSplit();
}

Expected<AccessSampleResult>
RandomMemoryAccessSampler::sample(size_t AccessSize, size_t Alignment,
                                  bool AllowMisalign,
                                  std::function<AddressGenInfo(MemoryAccess &)>
                                      ChooseAddrGenInfo, // FIXME: const?
                                  bool BurstMode) {
  auto &MAGWithSchemes = getMAG();
  auto SchemeExp = MAGWithSchemes.getValidAccesses(AccessSize, Alignment,
                                                   AllowMisalign, BurstMode);
  if (!SchemeExp)
    return Expected<AccessSampleResult>(SchemeExp.takeError());
  auto &Scheme = *SchemeExp;
  auto AddrGenInfo = ChooseAddrGenInfo(*Scheme);
  auto AI = Scheme->randomAddress(AddrGenInfo);
  assert(MB.contained(AI) && "Address Info potentially out of memory bank");
  return AccessSampleResult{AI, AddrGenInfo};
}

} // namespace snippy
} // namespace llvm
