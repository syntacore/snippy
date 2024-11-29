//===-- SelfcheckInfo.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_SELFCHECK_INFO_H
#define LLVM_TOOLS_SNIPPY_SELFCHECK_INFO_H

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/GenResult.h"
#include "snippy/Support/Utils.h"

namespace llvm {
namespace snippy {
struct SelfCheckInfo final {
  unsigned long long CurrentAddress;
  AsOneGenerator<bool, true, false> PeriodTracker;
};

struct SelfCheckMap {
  SelfCheckMap() : Map(){};

  DenseMap<MemAddr, MemAddr> Map;

  void addToSelfcheckMap(MemAddr Address, MemAddr Distance) {
    [[maybe_unused]] auto EmplaceResult = Map.try_emplace(Address, Distance);
    assert(EmplaceResult.second &&
           "This address has been already inserterd to map.");
  }

  template <typename SelfRange> static SelfCheckMap merge(SelfRange &&SR) {
    SelfCheckMap Ret;
    for (auto &&Map : SR)
      for (auto &&[Address, Distance] : Map)
        Ret.addToSelfcheckMap(Address, Distance);
    return Ret;
  }
};

extern template class GenResultT<SelfCheckMap>;

} // namespace snippy
} // namespace llvm
#endif
