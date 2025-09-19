//===-- MemAccessGenerator.h ------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Generate memory accesses according to specified weights.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <vector>

#define DEBUG_TYPE "mem-access-generator"

namespace llvm {
namespace snippy {

class MemoryAccess;
struct AddressInfo;

struct MemKey {
  size_t AccessSize;
  size_t Alignment;
  bool AllowMisalign;
  bool BurstMode;
};
inline bool operator<(const MemKey &Lhs, const MemKey &Rhs) {
  return std::tie(Lhs.AccessSize, Lhs.Alignment, Lhs.AllowMisalign,
                  Lhs.BurstMode) < std::tie(Rhs.AccessSize, Rhs.Alignment,
                                            Rhs.AllowMisalign, Rhs.BurstMode);
}

struct MemValue {
  std::vector<size_t> MapToReaIdx;
  std::discrete_distribution<size_t> MemDist;

  size_t generate() {
    size_t Idx = MemDist(RandEngine::engine());
    return MapToReaIdx[Idx];
  }

  size_t getSchemeIdx(size_t SchemeId) const {
    return MapToReaIdx[SchemeId % MapToReaIdx.size()];
  }
};

template <typename Iter> class MemoryAccessGenerator final {
  using MemTableIter = std::map<MemKey, MemValue>::iterator;

  Iter First;
  Iter Last;
  std::map<MemKey, MemValue> MemTable;

  Expected<MemTableIter> updateTable(MemKey Key) {
    MemValue MemVal;
    std::vector<double> MemWeights;

    auto Count = std::distance(First, Last);
    MemWeights.reserve(Count);
    MemVal.MapToReaIdx.reserve(Count);
    for (auto [Index, Value] : enumerate(make_range(First, Last))) {
      auto Weight = Value->Weight;
      if (Value->isLegal({Key.AccessSize, Key.Alignment, Key.AllowMisalign,
                          Key.BurstMode}) &&
          Weight > 0) {
        MemWeights.push_back(Weight);
        MemVal.MapToReaIdx.push_back(Index);
      }
    }

    if (MemWeights.size() == 0) {
      return make_error<MemoryAccessSampleError>(
          formatv("Memory schemes are to restrictive to generate access (size: "
                  "{0}, alignment: {1})",
                  Key.AccessSize, Key.AllowMisalign ? 1 : Key.Alignment));
    }

    MemVal.MemDist = std::discrete_distribution<size_t>(MemWeights.begin(),
                                                        MemWeights.end());
    return MemTable.emplace(Key, MemVal).first;
  }

public:
  MemoryAccessGenerator(Iter FirstIn, Iter LastIn)
      : First(FirstIn), Last(LastIn), MemTable() {}

  void update(Iter FirstIn, Iter LastIn) {
    First = FirstIn;
    Last = LastIn;
    MemTable.clear();
  }

  Expected<size_t> generate(size_t AccessSize, size_t Alignment,
                            bool AllowMisalign, bool BurstMode) {
    MemKey Key{AccessSize, Alignment, AllowMisalign, BurstMode};
    auto FindIter = MemTable.find(Key);
    if (FindIter == MemTable.end()) {
      auto FindIterExp = updateTable(Key);
      if (auto Err = FindIterExp.takeError())
        return std::move(Err);
      FindIter = *FindIterExp;
    }
    return FindIter->second.generate();
  }

  void print(llvm::raw_ostream &OS) const {
    OS << "Length: " << std::distance(First, Last) << "\n";
    OS << "Cached " << MemTable.size() << " tables.\n";
    for (const auto &MemElem : MemTable)
      OS << "AccessSize:\t" << MemElem.first.AccessSize << "\tAlignment:\t"
         << MemElem.first.Alignment << "\tAllowMisalign:\t"
         << MemElem.first.AllowMisalign << "\tBurstMode:\t"
         << MemElem.first.BurstMode << "\tAvailable schemes:\t"
         << MemElem.second.MapToReaIdx.size() << "\n";
  }

  void dump() const { print(dbgs()); }
};

template <typename ContT> class MemAccGenerator final {
  ContT Accesses;
  MemoryAccessGenerator<typename ContT::iterator> MAG;

public:
  MemAccGenerator(ContT Container)
      : Accesses{std::move(Container)}, MAG{Accesses.begin(), Accesses.end()} {}

  MemAccGenerator(MemAccGenerator<ContT> &&OldMAG)
      : Accesses{std::move(OldMAG.Accesses)},
        MAG{Accesses.begin(), Accesses.end()} {}

  MemAccGenerator<ContT> &operator=(MemAccGenerator<ContT> &&Rhs) {
    Accesses = std::move(Rhs.Accesses);
    MAG = MemoryAccessGenerator<typename ContT::iterator>{Accesses.begin(),
                                                          Accesses.end()};
    return *this;
  }

  Expected<const typename ContT::value_type &>
  getValidAccesses(size_t AccessSize, size_t Alignment, bool AllowMisalign,
                   bool BurstMode) {
    auto PickedScheme =
        MAG.generate(AccessSize, Alignment, AllowMisalign, BurstMode);
    if (auto Err = PickedScheme.takeError())
      return std::move(Err);
    assert(*PickedScheme < Accesses.size());
    return Accesses[*PickedScheme];
  }
};

// deduction guide for ContT
template <typename ContT>
MemAccGenerator(ContT, size_t) -> MemAccGenerator<ContT>;

} // namespace snippy
} // namespace llvm

#undef DEBUG_TYPE
