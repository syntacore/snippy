//===-- MemAccessSampler.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_GENERATOR_IMEM_ACCESS_SAMPLER_H
#define LLVM_SNIPPY_INCLUDE_GENERATOR_IMEM_ACCESS_SAMPLER_H

#include "snippy/Config/MemoryScheme.h"

namespace llvm {
namespace snippy {

struct AccessSampleResult final {
  AddressInfo AddrInfo;
  AddressGenInfo AddrGenInfo;
};

class IMemoryAccessSampler {
public:
  virtual ~IMemoryAccessSampler() = default;
  virtual Expected<AccessSampleResult>
  sample(size_t AccessSize, size_t Alignment,
         std::function<AddressGenInfo(MemoryAccess &)> ChooseAddrGenInfo,
         bool BurstMode = false) = 0;

  virtual void reserve(MemRange) {}

  virtual void add(std::unique_ptr<MemoryAccess> Acc) {}

  virtual std::string getName() const = 0;

  virtual void print(raw_ostream &OS) const = 0;

  void dump() const { print(outs()); };
};

} // namespace snippy
} // namespace llvm

#endif
