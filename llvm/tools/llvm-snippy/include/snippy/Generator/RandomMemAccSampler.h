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

namespace llvm {
namespace snippy {

template <typename Iter> class MemoryAccessGenerator final {
  struct MemKey {
    size_t AccessSize;
    size_t Alignment;
    bool AllowMisalign;
    bool BurstMode;
    size_t MinStride;

    friend bool operator<(const MemKey &Lhs, const MemKey &Rhs) {
      auto Tie = [](const MemKey &Key) {
        return std::tie(Key.AccessSize, Key.Alignment, Key.AllowMisalign,
                        Key.BurstMode, Key.MinStride);
      };
      return Tie(Lhs) < Tie(Rhs);
    }
  };

  struct MemValue {
    std::vector<size_t> MapToReaIdx;
    std::discrete_distribution<size_t> MemDist;

    size_t generate() {
      size_t Idx = MemDist(RandEngine::engine());
      return MapToReaIdx[Idx];
    }

    friend bool operator==(const MemValue &Lhs, const MemValue &Rhs) {
      return Lhs.MapToReaIdx == Rhs.MapToReaIdx && Lhs.MemDist == Rhs.MemDist;
    }
  };

  Iter First;
  Iter Last;

  using MemTable = std::map<MemKey, MemValue>;
  using MaxElementsMemValueMap = std::map<std::size_t, MemValue>;

  std::map<MemKey, MaxElementsMemValueMap> LegalAccesses;

  Expected<typename decltype(LegalAccesses)::iterator>
  updateTable(unsigned NumElements, MemKey Key) {
    MemValue MemVal;
    std::vector<double> MemWeights;

    auto Count = std::distance(First, Last);
    MemWeights.reserve(Count);
    MemVal.MapToReaIdx.reserve(Count);
    auto AllAccesses = enumerate(make_range(First, Last));
    auto ValidAccesses =
        make_filter_range(AllAccesses, [&Key, NumElements](const auto &Pair) {
          const auto &[Index, Value] = Pair;
          auto Weight = Value->Weight;
          return Weight > 0 && Value->isLegal({Key.AccessSize, Key.Alignment,
                                               Key.AllowMisalign, Key.BurstMode,
                                               NumElements, Key.MinStride});
        });

    for (auto [Index, Value] : ValidAccesses) {
      MemWeights.push_back(Value->Weight);
      MemVal.MapToReaIdx.push_back(Index);
    }

    if (MemWeights.size() == 0) {
      return make_error<MemoryAccessSampleError>(
          formatv("Memory schemes are to restrictive to generate access (size: "
                  "{0}, alignment: {1}, stride: {2}, number of elements: {3})",
                  Key.AccessSize, Key.AllowMisalign ? 1 : Key.Alignment,
                  Key.MinStride, NumElements));
    }

    MemVal.MemDist = std::discrete_distribution<size_t>(MemWeights.begin(),
                                                        MemWeights.end());

    if (auto AccessIter = LegalAccesses.find(Key);
        AccessIter != LegalAccesses.end()) {
      auto &NEltsMap = AccessIter->second;
      assert(!NEltsMap.empty());
      auto LastIter = std::prev(NEltsMap.end());
      assert(LastIter->first < NumElements);
      if (LastIter->second == MemVal) {
        auto NH = NEltsMap.extract(LastIter->first);
        NH.key() = NumElements;
        NEltsMap.insert(std::move(NH));
      } else {
        NEltsMap.emplace(NumElements, std::move(MemVal));
      }

      return AccessIter;
    }

    auto [It, Inserted] = LegalAccesses.emplace(
        Key, MaxElementsMemValueMap{{NumElements, std::move(MemVal)}});
    assert(Inserted);
    return It;
  }

public:
  MemoryAccessGenerator(Iter FirstIn, Iter LastIn)
      : First(FirstIn), Last(LastIn), LegalAccesses() {}

  void update(Iter FirstIn, Iter LastIn) {
    First = FirstIn;
    Last = LastIn;
    LegalAccesses.clear();
  }

  Expected<size_t> generate(const AddressGenInfo &AddrGenInfo) {
    auto NumElements = AddrGenInfo.NumElements;
    assert(NumElements);

    MemKey Key{
        AddrGenInfo.AccessSize,    AddrGenInfo.Alignment,
        AddrGenInfo.AllowMisalign, AddrGenInfo.BurstMode,
        AddrGenInfo.MinStride,
    };

    auto FoundKeyIter = LegalAccesses.find(Key);
    if (FoundKeyIter == LegalAccesses.end()) {
      auto FindIterExp = updateTable(NumElements, Key);
      if (FindIterExp)
        FoundKeyIter = *FindIterExp;
      else
        return std::move(FindIterExp.takeError());
    }

    // If the requested count is <= then anything that's already cached - we
    // know that we can generate it. Otherwise, we have to update the entry (or
    // possibly insert a new one).
    MaxElementsMemValueMap &NEltsMapForKey = FoundKeyIter->second;
    auto FoundForNElts = NEltsMapForKey.lower_bound(NumElements);
    if (FoundForNElts == NEltsMapForKey.end()) {
      if (auto Err = updateTable(NumElements, Key).takeError())
        return Err;
      FoundForNElts = NEltsMapForKey.find(NumElements);
      assert(FoundForNElts != NEltsMapForKey.end());
    }

    return FoundForNElts->second.generate();
  }

  void print(llvm::raw_ostream &OS) const {
    OS << "Length: " << std::distance(First, Last)
       << ", Cached: " << LegalAccesses.size() << "\n";
    for (const auto &[MemKey, ForSelectedNumElements] : LegalAccesses) {
      OS.indent(2) << "AccessSize: " << MemKey.AccessSize
                   << ", Alignment: " << MemKey.Alignment
                   << ", AllowMisalign: " << MemKey.AllowMisalign
                   << ", BurstMode: " << MemKey.BurstMode
                   << ", MinStride: " << MemKey.MinStride << "\n";
      for (const auto &[NumElements, MemValue] : ForSelectedNumElements)
        OS.indent(4) << "N = " << NumElements
                     << ", Available schemes: " << MemValue.MapToReaIdx.size()
                     << "\n";
    }
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
  getValidAccesses(const AddressGenInfo &AddrGenInfo) {
    auto PickedScheme = MAG.generate(AddrGenInfo);
    if (auto Err = PickedScheme.takeError())
      return std::move(Err);
    assert(*PickedScheme < Accesses.size());
    return Accesses[*PickedScheme];
  }
};

// deduction guide for ContT
template <typename ContT>
MemAccGenerator(ContT, size_t) -> MemAccGenerator<ContT>;

// TODO: Get rid of the template completely.
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
                            AccIt AccEnd, Align Alignment,
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
    auto AccRanges = llvm::map_range(Filtered, [Alignment](auto &S) {
      return std::make_unique<MemoryAccessRange>(S, Alignment.value());
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

  MemoryAccessesGenerator &getMAG();

  Expected<AddressInfo> sample(const AddressGenInfo &AddrGenInfo) override;

  Expected<MemoryAccess &>
  chooseAccess(const AddressGenInfo &AddrGenInfo) override;

  void print(raw_ostream &OS) const override;
};

} // namespace snippy
} // namespace llvm

#endif
