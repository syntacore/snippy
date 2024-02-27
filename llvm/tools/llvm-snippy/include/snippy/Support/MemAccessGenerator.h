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

#include "llvm/Support/raw_ostream.h"

#include "RandUtil.h"
#include "Utils.h"

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
  bool BurstMode;
};
inline bool operator<(const MemKey &Lhs, const MemKey &Rhs) {
  return std::tie(Lhs.AccessSize, Lhs.Alignment, Lhs.BurstMode) <
         std::tie(Rhs.AccessSize, Rhs.Alignment, Rhs.BurstMode);
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

  MemTableIter updateTable(MemKey Key) {
    MemValue MemVal;
    std::vector<double> MemWeights;

    auto Count = std::distance(First, Last);
    MemWeights.reserve(Count);
    MemVal.MapToReaIdx.reserve(Count);
    for (auto [Index, Value] : enumerate(make_range(First, Last))) {
      auto Weight = Value->Weight;
      if (Value->isLegal({Key.AccessSize, Key.Alignment, Key.BurstMode}) &&
          Weight > 0) {
        MemWeights.push_back(Weight);
        MemVal.MapToReaIdx.push_back(Index);
      }
    }

    if (MemWeights.size() == 0) {
      std::string ErrMsg;
      raw_string_ostream OS(ErrMsg);
      OS << "Memory schemes are too restrictive to generate the requested "
            "instructions."
         << " Size: " << Key.AccessSize << " Alignment: " << Key.Alignment
         << '\n';
      report_fatal_error(ErrMsg.c_str(), false);
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

  size_t generate(size_t AccessSize, size_t Alignment, bool BurstMode,
                  std::optional<size_t> PreselectedSchemeId = std::nullopt) {
    MemKey Key{AccessSize, Alignment, BurstMode};
    auto FindIter = MemTable.find(Key);
    if (FindIter == MemTable.end())
      FindIter = updateTable(Key);
    if (PreselectedSchemeId)
      return FindIter->second.getSchemeIdx(*PreselectedSchemeId);
    return FindIter->second.generate();
  }

  void print(llvm::raw_ostream &OS) const {
    OS << "Length: " << std::distance(First, Last) << "\n";
    OS << "Cached " << MemTable.size() << " tables.\n";
    for (const auto &MemElem : MemTable)
      OS << "AccessSize:\t" << MemElem.first.AccessSize << "\tAlignment:\t"
         << MemElem.first.Alignment << "\tBurstMode:\t"
         << MemElem.first.BurstMode << "\tAvailable schemes:\t"
         << MemElem.second.MapToReaIdx.size() << "\n";
  }

  void dump() const { print(dbgs()); }
};

template <typename ContT> class OwningMAG final {
  ContT Accesses;
  MemoryAccessGenerator<typename ContT::iterator> MAG;
  size_t MAGId;

public:
  // Id field should be unique only if this MAG is used in plugin interface
  OwningMAG(ContT Container, size_t Id = 0)
      : Accesses{std::move(Container)}, MAG{Accesses.begin(), Accesses.end()},
        MAGId{Id} {}

  OwningMAG(OwningMAG<ContT> &&OldMAG)
      : Accesses{std::move(OldMAG.Accesses)},
        MAG{Accesses.begin(), Accesses.end()}, MAGId{OldMAG.MAGId} {}

  OwningMAG<ContT> &operator=(OwningMAG<ContT> &&Rhs) {
    Accesses = std::move(Rhs.Accesses);
    MAG = MemoryAccessGenerator<typename ContT::iterator>{Accesses.begin(),
                                                          Accesses.end()};
    MAGId = Rhs.MAGId;
    return *this;
  }

  const typename ContT::value_type &
  getValidAccesses(size_t AccessSize, size_t Alignment, bool BurstMode,
                   std::optional<size_t> InstrClassId) {
    auto PickedScheme =
        MAG.generate(AccessSize, Alignment, BurstMode, InstrClassId);
    assert(PickedScheme < Accesses.size());
    return Accesses[PickedScheme];
  }

  size_t getId() const { return MAGId; }
};

// deduction guide for ContT
template <typename ContT> OwningMAG(ContT, size_t) -> OwningMAG<ContT>;

} // namespace snippy
} // namespace llvm

#undef DEBUG_TYPE
