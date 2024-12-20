//===-- TopMemAccessSampler.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_GENERATOR_TOP_MEM_ACCESS_SAMPLER_H
#define LLVM_SNIPPY_INCLUDE_GENERATOR_TOP_MEM_ACCESS_SAMPLER_H

#include "snippy/Generator/MemAccessSampler.h"

namespace llvm {
namespace snippy {

class SnippyProgramContext;

class TopLevelMemoryAccessSampler : public IMemoryAccessSampler {
  std::vector<std::unique_ptr<IMemoryAccessSampler>> Samplers;


public:
  void reserve(MemRange R) override {
    for (auto &S : Samplers)
      S->reserve(R);
  }

  void add(std::unique_ptr<MemoryAccess> A) override {
    for (auto &S : llvm::drop_end(Samplers))
      S->add(A->copy());
    Samplers.back()->add(std::move(A));
  }

  template <typename SamplerIter>
  TopLevelMemoryAccessSampler(SamplerIter Start, SamplerIter Finish
                              )
  {
    Samplers.insert(Samplers.end(), std::make_move_iterator(Start),
                    std::make_move_iterator(Finish));
  }


  Expected<AccessSampleResult>
  sample(size_t AccessSize, size_t Alignment,
         std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,
         std::optional<::AddressGlobalId> Preselected = std::nullopt,
         bool BurstMode = false) override;

  Expected<AccessSampleResult>
  sample(size_t AccessSize, size_t Alignment,
         bool BurstMode = false,
         std::optional<::AddressGlobalId> Preselected = std::nullopt) {
    auto ChooseGenInfo = [&](auto &&Scheme) {
      return AddressGenInfo::singleAccess(AccessSize, Alignment, BurstMode);
    };
    return sample(AccessSize, Alignment, ChooseGenInfo,
                  Preselected, BurstMode);
  }

  std::vector<AddressInfo>
  randomBurstGroupAddresses(ArrayRef<AddressRestriction> ARRange,
                            const OpcodeCache &OpcC,
                            const SnippyTarget &SnpTgt);

  std::string getName() const override {
    return "top-level memory access sampler";
  }

  void print(raw_ostream &OS) const override {
    OS << getName() << ":\n";
    for (auto &S : Samplers)
      S->print(OS);
  }
};

} // namespace snippy
} // namespace llvm

#endif
