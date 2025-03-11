//===-- RandomMemAccSampler.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_GENERATOR_RAND_MEM_ACCESS_SAMPLER_H
#define LLVM_SNIPPY_INCLUDE_GENERATOR_RAND_MEM_ACCESS_SAMPLER_H

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/MemAccessSampler.h"
#include "snippy/Support/MemAccessGenerator.h"

namespace llvm {
namespace snippy {
using MemoryAccessesGenerator = MemAccGenerator<std::vector<MemoryAccess *>>;

class RandomMemoryAccessSampler : public IMemoryAccessSampler {
  MemoryAccessSeq BaseAccesses;
  MemoryAccessSeq SplitAccesses;

  MemoryBank RestrictedMB;
  MemoryBank MB;

  MemoryAccessesGenerator MAG{std::vector<MemoryAccess *>{}};

private:
  void updateMAG();

  void updateMemoryBank() {
    MemoryBank NewMB;
    NewMB.addRange(MemRange{0, std::numeric_limits<MemAddr>::max()});
    MB = NewMB.diff(RestrictedMB);
    updateSplit();
  }

  void updateSplit() {
    SplitAccesses.clear();
    for (auto &MS : BaseAccesses) {
      auto Accesses = MS->split(MB);
      for (auto &A : Accesses)
        SplitAccesses.emplace_back(std::move(A));
    }
    updateMAG();
  }

public:
  template <typename SectIt, typename AccIt>
  RandomMemoryAccessSampler(SectIt SectBegin, SectIt SectEnd, AccIt AccBegin,
                            AccIt AccEnd,
                            std::optional<unsigned> Alignment = std::nullopt,
                            MemoryBank Restricted = {})
      : RestrictedMB(std::move(Restricted)) {
    auto Filtered = llvm::make_filter_range(
        llvm::make_range(SectBegin, SectEnd), [](auto &S) {
          return (!S.isNamed() ||
                  !SectionsDescriptions::isSpecializedSectionName(S.getName()));
        });

    auto Copies = llvm::map_range(llvm::make_range(AccBegin, AccEnd),
                                  [](auto &A) { return A.copy(); });
    add(Copies.begin(), Copies.end());
    if (!Copies.empty())
      return;
    assert(Alignment.has_value());
    auto AccRanges = llvm::map_range(Filtered, [Alignment](auto &S) {
      return std::make_unique<MemoryAccessRange>(S, *Alignment);
    });
    add(AccRanges.begin(), AccRanges.end());
  }

  void add(std::unique_ptr<MemoryAccess> Acc) override;

  template <typename It> void add(It Start, It Finish) {
    for (; Start != Finish; ++Start)
      BaseAccesses.emplace_back(*Start);
    updateMemoryBank();
  }

  void reserve(MemRange) override;

  std::string getName() const override {
    return "random memory access sampler";
  }

  Expected<AccessSampleResult>
  sample(size_t AccessSize, size_t Alignment,
         std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,

         bool BurstMode = false) override;

  MemoryAccessesGenerator &getMAG();

  void print(raw_ostream &OS) const override;
};

} // namespace snippy
} // namespace llvm

#endif
