//===-- MemoryScheme.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"

#include <bitset>

#define DEBUG_TYPE "snippy-memory-scheme"

namespace llvm {

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::MemoryBank, false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::AccessAddress, false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::AddressInfo, false);

namespace snippy {

namespace {

constexpr const size_t MaxAccessSize = 8;

struct NormalizedMemoryAccesses {
  NormalizedMemoryAccesses(yaml::IO &Io) {}

  NormalizedMemoryAccesses(yaml::IO &Io, MemoryAccessSeq &BaseAccesses) {
    for (auto &&Access : BaseAccesses) {
      switch (Access->getMode()) {
      case MemoryAccessMode::Range:
        Ranges.push_back(cast<MemoryAccessRange>(*Access));
        break;
      case MemoryAccessMode::Eviction:
        Evictions.push_back(cast<MemoryAccessEviction>(*Access));
        break;
      case MemoryAccessMode::Addresses:
        Addresses.push_back(cast<MemoryAccessAddresses>(*Access));
        break;
      }
    }
  }

  MemoryAccessSeq denormalize(yaml::IO &Io) {
    MemoryAccessSeq Seq;

    auto CopyToSeq = [Inserter = std::back_inserter(Seq)](auto Range) {
      using value_type = typename decltype(Range)::value_type;
      transform(Range, Inserter,
                [](auto &&Val) { return std::make_unique<value_type>(Val); });
    };

    CopyToSeq(Ranges);
    CopyToSeq(Evictions);
    CopyToSeq(Addresses);

    return Seq;
  }

  std::vector<MemoryAccessRange> Ranges;
  std::vector<MemoryAccessEviction> Evictions;
  std::vector<MemoryAccessAddresses> Addresses;
};

} // namespace

class NormalizedMemoryBaseAccessesAndAccessGroups {
  void extractMemSchemesFromAccessGroups(yaml::IO &Io,
                                         MemoryAccessSeq &BaseAccesses) {
    for (auto &AG : AccessGroups) {
      for (auto &MA : AG.BaseGroupAccesses) {
        MA->Weight *= AG.Weight;
        if (MA->Weight != std::numeric_limits<double>::infinity()) {
          BaseAccesses.push_back(std::move(MA));
          continue;
        }

        Io.setError("The weight of the memory scheme, after "
                    "calculation, became infinity");
        break;
      }
    }
  }

public:
  NormalizedMemoryBaseAccessesAndAccessGroups(yaml::IO &Io) : Accesses(Io) {}

  NormalizedMemoryBaseAccessesAndAccessGroups(yaml::IO &Io,
                                              MemoryAccessSeq &BaseAccesses)
      : Accesses(Io, BaseAccesses) {}

  MemoryAccessSeq denormalize(yaml::IO &Io) {
    MemoryAccessSeq BaseAccesses = Accesses.denormalize(Io);
    extractMemSchemesFromAccessGroups(Io, BaseAccesses);
    return BaseAccesses;
  }

  NormalizedMemoryAccesses Accesses;
  MemoryAccessesGroupSeq AccessGroups;
};

} // namespace snippy

template <> struct yaml::MappingTraits<snippy::AccessAddress> {
  static void mapping(yaml::IO &IO, snippy::AccessAddress &AA) {
    IO.mapRequired("addr", AA.Addr);
    IO.mapOptional("access-size", AA.AccessSize, 16);
  }
};

template <> struct yaml::MappingTraits<snippy::AddressInfo> {
  static void mapping(yaml::IO &Io, snippy::AddressInfo &AI) {
    if (!Io.outputting())
      AI.MinOffset = 0;

    Io.mapRequired("addr", AI.Address);
    Io.mapRequired("size", AI.MaxOffset);
    Io.mapRequired("stride", AI.MinStride);
    Io.mapRequired("access-size", AI.AccessSize);
  }
};

void yaml::MappingTraits<snippy::MemoryAccessRange>::mapping(
    yaml::IO &Io, snippy::MemoryAccessRange &Range) {
  Io.mapOptional("weight", Range.Weight);
  Io.mapRequired("start", Range.Start);
  Io.mapRequired("size", Range.Size);
  Io.mapRequired("stride", Range.Stride);
  Io.mapRequired("first-offset", Range.FirstOffset);
  Io.mapRequired("last-offset", Range.LastOffset);

  if (!Io.outputting())
    Range.initAllowedAlignmentLCBlockOffsets();
}

std::string yaml::MappingTraits<snippy::MemoryAccessRange>::validate(
    yaml::IO &Io, snippy::MemoryAccessRange &Range) {
  // TODO: Remove this garbage and replace with a proper diagnostic
  if (Range.FirstOffset > Range.LastOffset)
    errs() << "Warning: first offset " << Range.FirstOffset << " > last offset "
           << Range.LastOffset << "\n";
  if (Range.LastOffset >= Range.Stride)
    errs() << "Warning: last offset " << Range.LastOffset << " >= stride "
           << Range.Stride << "\n";

  if (Range.Stride == 0)
    return "Stride cannot be equal to 0";
  if (Range.Weight < 0)
    return "Range access weight can not be less than 0";
  return "";
}

void yaml::MappingTraits<snippy::MemoryAccessEviction>::mapping(
    yaml::IO &Io, snippy::MemoryAccessEviction &Eviction) {
  yaml::MappingNormalization<snippy::NormalizedYAMLStrongTypedef<yaml::Hex64>,
                             decltype(Eviction.Mask)>
      MaskNorm(Io, Eviction.Mask);

  yaml::MappingNormalization<snippy::NormalizedYAMLStrongTypedef<yaml::Hex64>,
                             decltype(Eviction.Mask)>
      FixedNorm(Io, Eviction.Fixed);

  Io.mapOptional("weight", Eviction.Weight);
  Io.mapRequired("mask", MaskNorm->Value);
  Io.mapRequired("fixed", FixedNorm->Value);
}

std::string yaml::MappingTraits<snippy::MemoryAccessEviction>::validate(
    yaml::IO &Io, snippy::MemoryAccessEviction &Eviction) {
  if (Eviction.Mask & Eviction.Fixed)
    return "Bits in mask and fixed fields for eviction overlap";
  if (Eviction.Weight < 0)
    return "Eviction access weight can not be less than 0";
  return "";
}

void yaml::MappingTraits<snippy::MemoryAccessAddresses>::mapping(
    yaml::IO &Io, snippy::MemoryAccessAddresses &Addresses) {
  // FIXME: Unbrick this code
  if (!Io.outputting()) {
    bool Ordered = true;
    Io.mapOptional("ordered", Ordered, true);
    if (Ordered) {
      Addresses.NextAddressIdx = 0;
      Addresses.NextBurstIdx = 0;
    }
  }

  Io.mapOptional("weight", Addresses.Weight);
  Io.mapOptional("plain", Addresses.Addresses);
  Io.mapOptional("burst", Addresses.Burst);
}

std::string yaml::MappingTraits<snippy::MemoryAccessAddresses>::validate(
    yaml::IO &Io, snippy::MemoryAccessAddresses &Addresses) {
  if (Addresses.Addresses.empty() && Addresses.Burst.empty())
    return "At least one address must be provided either in "
           "'plain' or 'burst' format for "
           "access-addresses memory scheme";
  if (Addresses.Weight < 0)
    return "Addresses access weight can not be less than 0";
  return "";
}

void yaml::MappingTraits<snippy::MemoryBank>::mapping(yaml::IO &Io,
                                                      snippy::MemoryBank &MB) {
  assert(!Io.outputting());
  std::vector<snippy::AccessAddress> AAs;
  Io.mapRequired("plain", AAs);
  for (auto &AA : AAs) {
    MB.addRange(snippy::MemRange{AA.Addr, AA.Addr + AA.AccessSize});
  }
}

static void mapBaseAccesses(yaml::IO &Io,
                            snippy::NormalizedMemoryAccesses &Norm) {
  Io.mapOptional("access-ranges", Norm.Ranges);
  Io.mapOptional("access-evictions", Norm.Evictions);
  Io.mapOptional("access-addresses", Norm.Addresses);
}

void yaml::MappingTraits<snippy::MemoryAccessesGroup>::mapping(
    yaml::IO &Io, snippy::MemoryAccessesGroup &MG) {
  yaml::MappingNormalization<snippy::NormalizedMemoryAccesses,
                             snippy::MemoryAccessSeq>
      MappingNorm(Io, MG.BaseGroupAccesses);
  // NOTE: Nothing to see here. It's really necessary to call .operator->()
  // because there's no .get() method unfortunately
  mapBaseAccesses(Io, *MappingNorm.operator->());
  Io.mapOptional("weight", MG.Weight);
}

std::string yaml::MappingTraits<snippy::MemoryAccessesGroup>::validate(
    yaml::IO &Io, snippy::MemoryAccessesGroup &MG) {
  if (MG.Weight < 0)
    return "Access-group weight can not be less than 0";
  return "";
}

void yaml::MappingTraits<snippy::MemoryAccesses>::mapping(
    yaml::IO &Io, snippy::MemoryAccesses &MA) {
  MappingNormalization<snippy::NormalizedMemoryBaseAccessesAndAccessGroups,
                       snippy::MemoryAccessSeq>
      MappingNorm(Io, MA.BaseAccesses);

  mapBaseAccesses(Io, MappingNorm->Accesses);
  Io.mapOptional("access-groups", MappingNorm->AccessGroups);

  // FIXME: Possibly serialize this field?
  if (!Io.outputting()) {
    std::vector<snippy::MemoryBank> MBs;
    Io.mapOptional("restricted-addresses", MBs);
    for (auto &MB : MBs) {
      MA.Restricted = MA.Restricted.unite(MB);
    }
  }
}

namespace snippy {
namespace {

auto getRangeFromSection(const SectionDesc &Sec) {
  return MemRange{Sec.VMA, Sec.VMA + Sec.Size};
}

} // namespace
bool MemoryBank::contained(MemRange R) const {
  auto LR = lowerRange(R);
  auto UR = upperRange(R);
  if (LR == UR)
    return false;
  auto &CR = *LR;
  return CR.Start <= R.Start && CR.End >= R.End;
}

bool MemoryBank::contained(AddressInfo AI) const {
  MemRange R{AI};
  return contained(R);
}

void MemoryBank::addRange(MemRange R) {
  auto LR = lowerRange(R);
  auto UR = upperRange(R);
  if (LR == UR) {
    // Range R does not intersect with current bank.
    // Add it as is.
    Ranges.insert(R);
    mergeRanges();
    return;
  }
  // Otherwise, erase intersecting regions and
  // insert combined range.
  //
  // Example:
  //              R
  //        [============]
  //        |            |
  //    [======]    [==] | [==]
  //    |  LR            |  UR
  //    |                |
  // Transofrms to:      |
  //    |                |
  //    [================] [==]
  //           New
  //
  MemRange New = MemRange{std::min(LR->Start, R.Start),
                          std::max(std::prev(UR)->End, R.End)};
  Ranges.erase(LR, UR);
  Ranges.insert(New);

  mergeRanges();
}

MemoryBank MemoryBank::unite(const MemoryBank &Rhs) const {
  MemoryBank Ret = *this;
  for (auto &R : Rhs.Ranges)
    Ret.addRange(R);
  return Ret;
}

MemoryBank MemoryBank::intersect(const MemoryBank &Rhs) const {
  MemoryBank Ret;
  for (auto &R : Ranges) {
    for (auto It = Rhs.lowerRange(R), End = Rhs.upperRange(R); It != End;
         ++It) {
      auto Common = R.intersect(*It);
      if (Common)
        Ret.Ranges.insert(Common);
    }
  }
  return Ret;
}

MemoryBank MemoryBank::diff(const MemoryBank &Rhs) const {
  MemoryBank Ret = *this;
  for (auto &R : Rhs.Ranges) {
    Ret.substractRange(R);
  }
  return Ret;
}

void MemoryBank::substractRange(MemRange R) {
  auto LR = lowerRange(R);
  auto UR = upperRange(R);
  if (LR == UR)
    return;
  while (LR != UR) {
    auto CR = *LR;
    if (CR.Start < R.Start) {
      if (CR.End > R.End) {
        // Restricted region is fully covered by
        // current region.
        Ranges.erase(LR);
        Ranges.emplace(CR.Start, R.Start);
        LR = std::next(Ranges.emplace(R.End, CR.End).first);
      } else {
        // Restricted region start overlaps with
        // current region end.
        Ranges.erase(LR);
        LR = std::next(Ranges.emplace(CR.Start, R.Start).first);
      }
    } else {
      if (CR.End > R.End) {
        // Restricted region end overlaps with
        // current region start.
        Ranges.erase(LR);
        LR = std::next(Ranges.emplace(R.End, CR.End).first);
      } else {
        // Restricted region fully covers current
        // region.
        LR = Ranges.erase(LR);
      }
    }
  }
}

// This method merges consecutive ranges if there is no
// space between them (second range starts where first ends).
//
//   E.G.:
//
//       R1       R2       R3
//   [========][=======]  [==]
//
//           R4            R3
//   [=================]  [==]
//
//   R1 and R2 are replaced by R4
void MemoryBank::mergeRanges() {
  auto R = Ranges.begin();

  // Advance R to next concerned pair.
  auto Next = [&R, this]() {
    R = std::adjacent_find(
        R, Ranges.end(), [](auto &R1, auto &R2) { return R1.End == R2.Start; });
    return R != Ranges.end();
  };

  while (Next()) {
    // Create merged range for concerned pair.
    MemRange MergedRange{R->Start, std::next(R)->End};
    // Erase old pair.
    Ranges.erase(R, std::next(R, 2));
    // Next scan start from newly inserted range.
    R = Ranges.emplace(MergedRange).first;
  }
}

void MemoryAccessRange::initAllowedAlignmentLCBlockOffsets() {
  for (auto &&[Idx, AllowedLCBlockOffsets] :
       enumerate(AlignmentAllowedLCBlockOffsets)) {
    // An offset into this memory section is valid if
    // it's properly aligned and its offset in a Stride-wide block
    // is within [FirstOffset; LastOffset]
    assert(Idx < std::numeric_limits<size_t>::digits);
    auto Alignment = size_t(1) << Idx;
    auto LCStride = std::lcm(Stride, Alignment);
    auto FirstAlignedOffset = alignTo(Start, Alignment) - Start;
    for (size_t Offset = FirstAlignedOffset; Offset < LCStride;
         Offset += Alignment) {
      auto BlockOffset = Offset % Stride;
      if (FirstOffset <= BlockOffset && BlockOffset <= LastOffset)
        AllowedLCBlockOffsets.push_back(Offset);
    }
  }
}

MemoryBank MemoryAccessRange::getPossibleAddresses() const {
  MemoryBank MB;
  // FIXME: This is not entirely correct because range can have holes.
  MB.addRange(MemRange{Start, Start + Size});
  return MB;
}

MemoryAccessSeq MemoryAccessRange::split(const MemoryBank &MB) const {
  MemoryAccessSeq Ret;
  auto WholeRange = MemRange{Start, Start + Size};
  for (auto &R : MB) {
    if (!WholeRange.interfere(R))
      continue;
    auto Intersected = WholeRange.intersect(R);
    auto NewStart = Start + alignTo(Intersected.Start - Start, Stride);
    if (NewStart > Intersected.End)
      continue;
    auto NewMemAccess = std::make_unique<MemoryAccessRange>();
    NewMemAccess->Start = NewStart;
    NewMemAccess->Stride = Stride;
    NewMemAccess->FirstOffset = FirstOffset;
    NewMemAccess->LastOffset = LastOffset;
    NewMemAccess->Size = Intersected.End - NewMemAccess->Start;

    NewMemAccess->Weight = Size != 0 ? Weight * NewMemAccess->Size / Size : 0;
    NewMemAccess->initAllowedAlignmentLCBlockOffsets();
    Ret.emplace_back(std::move(NewMemAccess));
  }
  return Ret;
}

bool MemoryAccessRange::isLegal(const AddressGenInfo &AddrGenInfo) const {
  auto Alignment = AddrGenInfo.Alignment;
  auto AccessSize = AddrGenInfo.AccessSize;
  auto MinStride = AddrGenInfo.MinStride;
  auto LCStride = getLCStride(Alignment);

  assert(isPowerOf2_64(Alignment) && Alignment <= 8);

  if (AccessSize > Size)
    return false;

  auto MaxOffset = Size - AccessSize;
  const auto &AllowedLCBlockOffsets =
      AlignmentAllowedLCBlockOffsets[Log2_64(Alignment)];

  if (AllowedLCBlockOffsets.empty() ||
      // FIXME: Is it actually possible? Seems like this
      // would be possible only when LCStride is greater
      // than Size
      AllowedLCBlockOffsets.front() > MaxOffset)
    return false;

  // Nothing more to check for a single address generation
  if (AddrGenInfo.isSingleElement())
    return true;

  // Here we check whether it's possible to generate a strided address for
  // multiple elements
  auto NumElements = AddrGenInfo.NumElements;
  if (LCStride < MinStride)
    return false;

  // MaxLCBlock is the last LCStride-wide block that element can fit in. Then
  // the total number of elements that can be addressed with LCStride is
  // MaxLCBlock + 1
  auto MaxLCBlock = getMaxLCBlock(Alignment, AccessSize);
  return NumElements <= MaxLCBlock + 1;
}

AddressInfo MemoryAccessRange::randomAddress(const AddressGenInfo &Params) {
  assert(isLegal(Params));

  auto Alignment = Params.Alignment;
  auto AccessSize = Params.AccessSize;
  auto LCStride = getLCStride(Alignment);
  auto NumElements = Params.NumElements;
  auto PreselectedAddr = Params.PreselectedAddr;

  auto &AllowedLCBlockOffsets =
      AlignmentAllowedLCBlockOffsets[Log2_64(Alignment)];
  assert(!AllowedLCBlockOffsets.empty());

  auto MaxOffset = Size - AccessSize;

  // Special care needs to be taken to select an address in the appropriate
  // block so that all NumElements values can fit. Note: beware off-by-one
  // errors. If NumElements == 1, then maxLCBlock = getMaxLCBlock();
  auto MaxLCBlock = getMaxLCBlock(Alignment, AccessSize) - (NumElements - 1);
  LLVM_DEBUG(dbgs() << "Numelements: " << NumElements
                    << ", MaxLCBlock: " << MaxLCBlock << "\n");
  // NOTE: The way that addresses are sampled is two-step. First select any
  // legal LCStride-wide block and some allowed Offset inside this block is
  // selected. Due to this it's doubtful that the distribution will be uniform
  // for complex scenarios, especially when Stride is not a a multiple of
  // alignment
  auto LCBlockIdx = snippy::RandEngine::genInInterval(MaxLCBlock);
  auto LCBlockOffsetIdx =
      snippy::RandEngine::genInRange(AllowedLCBlockOffsets.size());
  if (PreselectedAddr) {
    LCBlockIdx = PreselectedAddr->MainId % (MaxLCBlock + 1);
    LCBlockOffsetIdx = PreselectedAddr->OffsetId % AllowedLCBlockOffsets.size();
  }

  // FIXME: Maybe LCBlockOffsetIdx could be randomly sampled so this fixup would
  // not be necessary?
  auto Offset = LCBlockIdx * LCStride + AllowedLCBlockOffsets[LCBlockOffsetIdx];
  if (Offset > MaxOffset) {
    auto Slice =
        ArrayRef<size_t>(AllowedLCBlockOffsets).take_front(LCBlockOffsetIdx);
    auto LCBlockOffsetIt =
        std::find_if(Slice.rbegin(), Slice.rend(), [&](size_t AllowedOffset) {
          return LCBlockIdx * LCStride + AllowedOffset <= MaxOffset;
        });
    assert(LCBlockOffsetIt < Slice.rend());
    Offset = LCBlockIdx * LCStride + *LCBlockOffsetIt;
  }

  auto MinOffAligned = alignDown(Offset, LCStride);

  AddressInfo AI;
  AI.Address = Start + Offset;
  AI.MaxOffset = alignDown(Size - Offset - AccessSize, LCStride);
  AI.MinOffset = -static_cast<int long long>(MinOffAligned);
  AI.MinStride = LCStride;
  AI.AccessSize = AccessSize;

  LLVM_DEBUG(dbgs() << "Offset: " << Offset << ", LCBlockIdx: " << LCBlockIdx
                    << "\n");

  assert(AI.MaxOffset >= 0);
  assert(AI.MinOffset <= 0);

  return AI;
}

AddressInfo
MemoryAccessEviction::randomAddress(const AddressGenInfo &AddrGenInfo) {
  assert(isLegal(AddrGenInfo));

  auto Alignment = AddrGenInfo.Alignment;
  auto AccessSize = AddrGenInfo.AccessSize;
  auto PreselectedAddr = AddrGenInfo.PreselectedAddr;

  MemAddr Addr =
      snippy::RandEngine::genInRange(std::numeric_limits<MemAddr>::max());
  if (PreselectedAddr)
    Addr = PreselectedAddr->MainId;
  // Account for alignment in mask
  auto Mask = this->Mask & ~(Alignment - 1);
  Addr &= Mask;
  Addr |= Fixed;
  AddressInfo AI;
  AI.Address = Addr;
  AI.MaxOffset = 0;
  AI.MinOffset = 0;
  AI.MinStride = 0;
  AI.AccessSize = AccessSize;
  if (Mask == 0)
    return AI;

  // When Mask is not all zeros we can find minimal legal offsets and a
  // stride. For example:
  //   Mask  = 0b0101100
  //   Fixed = 0b0000001
  //   Addr  = 0b0101001
  //
  // Then max negative offset is 0b0001000 that gives min addr = 0b0100001,
  // and max positive offset is 0b0000100 that gives max addr = 0b0101101.
  // Stride is 0b0000100.
  //
  // Resulting addresses are:
  //   0b0100001, 0b0100101, 0b0101001, 0b0101101.

  // Find first sequence of ones in the mask starting from the least
  // significant bit. For the example above it will be
  //
  //   LastSet  First Set
  //         | |
  //     0b0101100
  auto FirstSet = countr_zero(Mask);
  auto TrailingZerosMask = maskTrailingOnes<MemAddr>(FirstSet);
  auto LastSet = countr_one(Mask | TrailingZerosMask);

  // We can get min addr from the original addr by zeroing bits from (LastSet,
  // FirstSet].
  auto DropTrailingOnesMask = maskTrailingZeros<MemAddr>(LastSet);
  auto MinAddr = (Addr & DropTrailingOnesMask) | Fixed;
  AI.MinOffset = MinAddr - Addr;
  assert(AI.MinOffset <= 0);
  // Mask addr also can be computed from the original addr by setting all bits
  // from (LastSet, FirstSet].
  auto AddTrailingOnesMask = maskTrailingOnes<MemAddr>(LastSet);
  auto MaskPart = Mask & AddTrailingOnesMask;
  auto MaxAddr = Addr | MaskPart;
  AI.MaxOffset = MaxAddr - Addr;
  assert(AI.MaxOffset >= 0);
  // Min stride has only one bit set at index FirstSet as it is the minimal
  // legal address step according to the mask.
  AI.MinStride = 1ull << FirstSet;
  assert(AI.MinOffset % AI.MinStride == 0);
  assert(AI.MaxOffset % AI.MinStride == 0);
  return AI;
}

MemoryBank MemoryAccessEviction::getPossibleAddresses() const {
  MemoryBank MB;
  MB.addRange(MemRange{Fixed, Fixed + Mask + MaxAccessSize});
  return MB;
}

class BitsTuple {
  std::bitset<sizeof(MemAddr) * CHAR_BIT> Mask, CurrMask;

public:
  BitsTuple(MemAddr MaskAddr) { Mask = MaskAddr; }

  // Details may be found in Knuth, algorithm M from 7.2.1.1 (vol 4A)
  MemAddr getNextTuple() {
    unsigned Bit = Mask.size();

    while ((Bit > 0) && (CurrMask[Bit - 1] == Mask[Bit - 1])) {
      CurrMask.reset(Bit - 1);
      Bit--;
    }

    if (Bit == 0)
      return 0;

    CurrMask.set(Bit - 1);
    return CurrMask.to_ullong();
  }
};

// Iterates over all tuples of Mask
static std::set<MemAddr> addAddrsForAllTuples(MemAddr Mask, MemAddr Fixed,
                                              MemAddr Start, MemAddr End) {
  MemAddr Addr = 0;
  BitsTuple CurrTuple(Mask);
  std::set<MemAddr> MissedAddrs;
  do {
    Addr |= Fixed;
    if ((Addr >= Start) && (Addr <= End - MaxAccessSize))
      MissedAddrs.insert(Addr);
  } while ((Addr = CurrTuple.getNextTuple()));

  return MissedAddrs;
}

MemoryAccessSeq MemoryAccessEviction::split(const MemoryBank &MB) const {
  MemoryAccessSeq Ret;
  auto WholeRange = MemRange{Fixed, Fixed + Mask + MaxAccessSize};
  double WholeNewWeight = 0;
  for (auto &R : MB) {
    if (!WholeRange.interfere(R))
      continue;
    auto Intersected = WholeRange.intersect(R);
    auto NewMemAccess = std::make_unique<MemoryAccessEviction>();
    NewMemAccess->Fixed = Fixed;
    NewMemAccess->Mask = 0u;
    auto CurrentBit = countr_zero(Mask);
    int MaxBit = sizeof(Mask) * CHAR_BIT - countr_zero(Mask);
    for (; CurrentBit < MaxBit; ++CurrentBit) {
      auto CurrentBitMask = (MemAddr(1) << CurrentBit);
      // look for next mask bit.
      if (!(Mask & CurrentBitMask))
        continue;
      // lowest possible value with current fixed and mask.
      auto LowestPossible = NewMemAccess->Fixed;
      if (LowestPossible < Intersected.Start) {
        // Move bit from mask to fixed.
        NewMemAccess->Fixed = NewMemAccess->Fixed | CurrentBitMask;
        continue;
      }
      // Overshoot range, no need to continue
      if (LowestPossible > Intersected.End - MaxAccessSize)
        break;
      auto HighestPossible =
          LowestPossible + NewMemAccess->Mask + CurrentBitMask;
      if (HighestPossible > Intersected.End - MaxAccessSize) {
        // All next bits need to be dropped from mask.
        break;
      }
      // finally, here bit can be left in mask.
      NewMemAccess->Mask |= CurrentBitMask;
    }

    if (NewMemAccess->Fixed < Intersected.Start ||
        NewMemAccess->Fixed > Intersected.End - MaxAccessSize)
      continue;

    // Add all missing addresses as one by one
    auto DiffFixed = Fixed ^ NewMemAccess->Fixed;
    std::set<MemAddr> MissedAddrs;
    if (DiffFixed) {
      // Processing all fixed bits in NewMemAccess->Fixed that can be 0, but
      // still generate correct addresses
      MissedAddrs = addAddrsForAllTuples(Mask | DiffFixed, Fixed,
                                         Intersected.Start, Intersected.End);
    }

    auto MaskAddrCount = popcount(NewMemAccess->Mask);
    NewMemAccess->Weight = static_cast<double>(1ull << MaskAddrCount);
    WholeNewWeight += NewMemAccess->Weight;
    Ret.emplace_back(std::move(NewMemAccess));
    for (auto Addr : MissedAddrs) {
      assert((Addr >= Intersected.Start) &&
             (Addr <= Intersected.End - MaxAccessSize));
      // If Addr can be generated by NewMemAccess we don't need to add it
      if ((Addr & (Fixed | DiffFixed)) == (Fixed | DiffFixed))
        continue;
      auto MissAddr = std::make_unique<MemoryAccessEviction>();
      MissAddr->Fixed = Addr;
      MissAddr->Mask = 0;
      Ret.emplace_back(std::move(MissAddr));
    }
    MissedAddrs.clear();
  }
  for (auto &R : Ret)
    R->Weight = WholeNewWeight != 0 ? Weight * R->Weight / WholeNewWeight : 0;

  return Ret;
}

static bool isLegalAddress(MemAddr Address, size_t Alignment) {
  return MinAlign(Address, Alignment) == Alignment;
}

static bool isLegalAddress(const AddressInfo &AI, size_t AccessSize,
                           size_t Alignment) {
  assert(isPowerOf2_64(Alignment));
  if (AI.AccessSize < AccessSize || AI.Address % Alignment != 0 ||
      static_cast<unsigned long long>(AI.MaxOffset) < AccessSize)
    return false;
  return AI.MinStride == 0 || std::max(Alignment, AI.MinStride) %
                                      std::min(Alignment, AI.MinStride) ==
                                  0;
}

AddressInfo MemoryAccessAddresses::randomAddressForPlainAccess(
    size_t AccessSize, size_t Alignment,
    std::optional<::AddressId> PreselectedAddr) {
  AddressInfo AI;
  if (NextAddressIdx && PreselectedAddr)
    report_fatal_error("Can't generate preselected address "
                       "for ordered memory scheme",
                       false);

  if (NextAddressIdx) {
    AI.Address = Addresses[*NextAddressIdx].Addr;
    NextAddressIdx = (*NextAddressIdx + 1) % Addresses.size();
  } else {
    SmallVector<AccessAddress, 32> LegalAddresses;
    copy_if(Addresses, std::back_inserter(LegalAddresses),
            [Alignment](auto Addr) {
              return isLegalAddress(Addr.Addr, Alignment);
            });
    assert(!LegalAddresses.empty() && "At least one address must be legal. "
                                      "We should've already checked it.");
    auto LegalAddressIdx = RandEngine::genInRange(LegalAddresses.size());
    if (PreselectedAddr)
      LegalAddressIdx = PreselectedAddr->MainId % LegalAddresses.size();
    AI.Address = LegalAddresses[LegalAddressIdx].Addr;
  }
  AI.MaxOffset = 0;
  AI.MinOffset = 0;
  AI.MinStride = 0;
  AI.AccessSize = AccessSize;
  return AI;
}

AddressInfo MemoryAccessAddresses::randomAddressForBurstAccess(
    size_t AccessSize, size_t Alignment,
    std::optional<::AddressId> PreselectedAddr) {
  if (NextBurstIdx && PreselectedAddr)
    report_fatal_error("Can't generate preselected address "
                       "for ordered memory scheme",
                       false);

  if (NextBurstIdx) {
    for (size_t i = 0; i < Burst.size(); ++i) {
      auto Idx = (i + *NextBurstIdx) % Burst.size();
      if (!isLegalAddress(Burst[Idx], AccessSize, Alignment))
        continue;
      NextBurstIdx = (Idx + 1) % Burst.size();
      return Burst[Idx];
    }
    llvm_unreachable(
        "This scheme was chosen as a legal one. Why it is not legal now?");
  }

  SmallVector<AddressInfo, 32> AIs;
  copy_if(Burst, std::back_inserter(AIs),
          [AccessSize, Alignment](const auto &AI) {
            return isLegalAddress(AI, AccessSize, Alignment);
          });
  assert(!AIs.empty() && "At least one entry must exist as we've already "
                         "checked the legality of the scheme.");
  auto AIIdx = RandEngine::genInRange(AIs.size());
  if (PreselectedAddr)
    AIIdx = PreselectedAddr->MainId % AIs.size();
  return AIs[AIIdx];
}

AddressInfo
MemoryAccessAddresses::randomAddress(const AddressGenInfo &AddrGenInfo) {
  assert(isLegal(AddrGenInfo));

  auto AccessSize = AddrGenInfo.AccessSize;
  auto Alignment = AddrGenInfo.Alignment;

  if (!AddrGenInfo.BurstMode)
    return randomAddressForPlainAccess(AccessSize, Alignment,
                                       AddrGenInfo.PreselectedAddr);
  auto AI = randomAddressForBurstAccess(AccessSize, Alignment,
                                        AddrGenInfo.PreselectedAddr);
  assert(static_cast<unsigned long long>(AI.MaxOffset) >= AccessSize);
  AI.MaxOffset -= AccessSize;
  AI.AccessSize = AccessSize;
  return AI;
}

bool MemoryAccessAddresses::isLegal(const AddressGenInfo &AddrGenInfo) const {
  auto AccessSize = AddrGenInfo.AccessSize;
  auto Alignment = AddrGenInfo.Alignment;

  assert(isPowerOf2_64(Alignment));

  if (!AddrGenInfo.isSingleElement())
    return false;

  if (AddrGenInfo.BurstMode) {
    return any_of(Burst, [&](const auto &AI) {
      return isLegalAddress(AI, AccessSize, Alignment);
    });
  }

  // We expect that provided addresses are legal for any instruction from
  // layout. It means that any AccessSize is legal.
  if (NextAddressIdx)
    return isLegalAddress(Addresses[*NextAddressIdx].Addr, Alignment);
  return any_of(Addresses, [Alignment](auto Addr) {
    return isLegalAddress(Addr.Addr, Alignment);
  });
}

MemoryBank MemoryAccessAddresses::getPossibleAddresses() const {
  MemoryBank MB;
  for (auto &Addr : Addresses) {
    MB.addRange(MemRange{Addr.Addr, Addr.Addr + MaxAccessSize});
  }

  for (auto &B : Burst) {
    MB.addRange(MemRange{B});
  }

  return MB;
}

MemoryAccessAddresses
MemoryAccessAddresses::splitPlainAccesses(const MemoryBank &MB) const {
  if (Addresses.empty())
    return {};

  // Addresses are sorted in ascending order of Addr field.
  // After some filtering, remaining addresses should be resorted
  // back in order they were. To do that - keep their indices along
  // via enumerate.
  struct EnumeratedAddr {
    size_t Idx;
    AccessAddress Value;
    EnumeratedAddr(size_t Idx, AccessAddress Value) : Idx(Idx), Value(Value){};
  };
  std::vector<EnumeratedAddr> SortedAddresses;
  for (auto &&[Idx, Addr] : llvm::enumerate(Addresses))
    SortedAddresses.emplace_back(Idx, Addr);

  assert(!SortedAddresses.empty());
  std::sort(SortedAddresses.begin(), SortedAddresses.end(),
            [](auto &Addr1, auto &Addr2) {
              return Addr1.Value.Addr < Addr2.Value.Addr;
            });
  auto EndRange = std::accumulate(
      SortedAddresses.begin(), SortedAddresses.end(), MemAddr{0u},
      [](auto End, auto &Addr) {
        return std::max(End, Addr.Value.Addr + Addr.Value.AccessSize);
      });
  auto WholeRange = MemRange{SortedAddresses.front().Value.Addr, EndRange};
  decltype(SortedAddresses) LegalAddresses;
  for (auto &R : MB) {
    if (!WholeRange.interfere(R))
      continue;
    auto Intersected = WholeRange.intersect(R);
    auto FirstInRange =
        std::lower_bound(SortedAddresses.begin(), SortedAddresses.end(),
                         Intersected.Start, [](auto &Addr, auto InterStart) {
                           return Addr.Value.Addr < InterStart;
                         });
    auto LastInRange = std::upper_bound(
        SortedAddresses.begin(), SortedAddresses.end(), Intersected.End,
        [](auto InterEnd, auto &Addr) { return Addr.Value.Addr > InterEnd; });
    // Copy only addresses which access size range is fully contained inside
    // Intersected.
    std::copy_if(FirstInRange, LastInRange, std::back_inserter(LegalAddresses),
                 [&Intersected](auto &Addr) {
                   return Addr.Value.Addr + Addr.Value.AccessSize <=
                          Intersected.End;
                 });
    // All those addresses are no longer needed to check as they
    // cannot be inside any other range of MB.
    SortedAddresses.erase(FirstInRange, LastInRange);
  }

  // Sort back.
  std::sort(LegalAddresses.begin(), LegalAddresses.end(),
            [](auto &Addr1, auto &Addr2) { return Addr1.Idx < Addr2.Idx; });

  if (LegalAddresses.empty())
    return {};

  MemoryAccessAddresses NewAddresses;
  std::transform(LegalAddresses.begin(), LegalAddresses.end(),
                 std::back_inserter(NewAddresses.Addresses),
                 [](auto &Addr) { return Addr.Value; });
  // If this scheme is ordered, make new ordered too.
  if (NextAddressIdx)
    NewAddresses.NextAddressIdx = 0u;
  return NewAddresses;
}

MemoryAccessAddresses
MemoryAccessAddresses::splitBurstAccesses(const MemoryBank &MB) const {
  if (Burst.empty())
    return {};

  MemoryAccessAddresses NewAddresses;
  assert((!NextBurstIdx || *NextBurstIdx == 0) &&
         "split is expected to be called before the first use of the scheme");
  NewAddresses.NextAddressIdx = NextAddressIdx;
  for (const auto &BurstAI : Burst) {
    auto Start = BurstAI.Address;
    auto Size = BurstAI.MaxOffset;
    auto Stride = BurstAI.MinStride;
    auto WholeRange = MemRange{Start, Start + Size};
    for (const auto &R : MB) {
      if (!WholeRange.interfere(R))
        continue;
      auto Intersected = WholeRange.intersect(R);
      auto NewStart =
          Start + alignTo(Intersected.Start - Start, (Stride ? Stride : 1));
      if (NewStart > Intersected.End)
        continue;
      auto NewAI = BurstAI;
      NewAI.Address = NewStart;
      NewAI.MaxOffset = Intersected.End - NewStart;
      NewAddresses.Burst.push_back(std::move(NewAI));
    }
  }

  return NewAddresses;
}

static size_t AccumulateAddressSize(const std::vector<AddressInfo> &VAI) {
  return std::accumulate(
      VAI.begin(), VAI.end(), 0ull,
      [](size_t Sum, const AddressInfo &Lhs) { return Lhs.MaxOffset + Sum; });
}

MemoryAccessSeq MemoryAccessAddresses::split(const MemoryBank &MB) const {
  auto PlainAccesses = splitPlainAccesses(MB);
  auto BurstAccesses = splitBurstAccesses(MB);

  MemoryAccessSeq Ret;
  if (PlainAccesses.Addresses.empty() && BurstAccesses.Burst.empty())
    return Ret;

  auto NewAccesses = std::make_unique<MemoryAccessAddresses>();

  // We split plain access which access size range is fully contained
  //   inside intersected. So, we should calculate new plain weight
  //   according to count of possible plain accesses.
  double PlainWeightNew = static_cast<double>(PlainAccesses.Addresses.size());
  double PlainWeightDiff =
      Addresses.size() != 0 ? PlainWeightNew / Addresses.size() : 1;
  // We can split one burst access into several if restricted address
  //   inside burst region. Thus we should calculate new burst weight
  //   according to total size of burst access regions.
  double BurstWeightNew =
      static_cast<double>(AccumulateAddressSize(BurstAccesses.Burst));
  double BurstWeightDiff =
      Burst.size() != 0 ? BurstWeightNew / AccumulateAddressSize(Burst) : 1;

  NewAccesses->Weight = Weight * PlainWeightDiff * BurstWeightDiff;
  NewAccesses->Addresses = std::move(PlainAccesses.Addresses);
  NewAccesses->Burst = std::move(BurstAccesses.Burst);
  NewAccesses->NextAddressIdx = PlainAccesses.NextAddressIdx;
  NewAccesses->NextBurstIdx = BurstAccesses.NextBurstIdx;
  Ret.emplace_back(std::move(NewAccesses));
  return Ret;
}

void MemoryAccessRange::print(raw_ostream &OS) const {
  OS << "Memory range:\n"
     << "  Weight: " << floatToString(Weight, 3) << "\n"
     << "  Start: " << Start << "\n"
     << "  Size: " << Size << "\n"
     << "  Stride: " << Stride << "\n"
     << "  FirstOffset: " << FirstOffset << "\n"
     << "  LastOffset: " << LastOffset << "\n";
  OS << "\n";
}

void MemoryAccessEviction::print(raw_ostream &OS) const {
  OS << "Memory eviction:\n  Weight: " << floatToString(Weight, 3)
     << "\n  Mask:  0x" << Twine::utohexstr(Mask) << "\n  Fixed: 0x"
     << Twine::utohexstr(Fixed) << "\n";
  OS << "\n";
}

void MemoryAccessAddresses::print(raw_ostream &OS) const {
  OS << "Memory addresses:\n"
     << "  Weight: " << floatToString(Weight, 3)
     << "\n  Ordered: " << NextAddressIdx.has_value() << "\n";
  if (!Addresses.empty())
    OS << "  Plain:\n";
  for (auto Addr : Addresses) {
    OS << "   - Addr: 0x" << Twine::utohexstr(Addr.Addr) << "\n";
    OS << "     Access-size: " << Addr.AccessSize << "\n";
  }
  if (!Burst.empty())
    OS << "  Burst:\n";
  for (const auto &AI : Burst) {
    OS << "    - Addr: 0x" << Twine::utohexstr(AI.Address) << "\n";
    OS << "      Size: " << AI.MaxOffset << "\n";
    OS << "      Stride: " << AI.MinStride << "\n";
    OS << "      Access-size: " << AI.AccessSize << "\n";
  }
  OS << "\n\n";
}

void MemoryAccesses::print(raw_ostream &OS) const {
  for (auto &&[Idx, Access] : enumerate(SplitAccesses)) {
    OS << "Memory access " << Idx << ":\n";
    Access->print(OS);
    OS << "\n";
  }
}

void MemoryScheme::print(raw_ostream &OS) const {
  OS << "Memory access scheme:\n";
  MA.print(OS);
}

void MemoryAccesses::loadFromYaml(LLVMContext &Ctx, StringRef Filename) {
  auto Err = loadYAMLFromFile(*this, Filename);
  if (Err)
    snippy::fatal(Ctx,
                  "Cannot read memory scheme " + Twine(Filename) + ". YAML",
                  toString(std::move(Err)));
}

void MemoryAccesses::validateSchemes(LLVMContext &Ctx,
                                     const SectionsDescriptions &Sections,
                                     bool StrictCheck) const {

  MemoryBank SecMB;
  for (auto &S : Sections.generalRWSections()) {
    SecMB.addRange(getRangeFromSection(S));
  }

  for (auto &MA : SplitAccesses) {

    auto MB = MA->getPossibleAddresses();
    if (MB.containedIn(SecMB))
      continue;

    std::string SchemeDump;
    raw_string_ostream SS{SchemeDump};
    MA->print(SS);

    constexpr const char *DiagPrefix = "Possibly wrong memory scheme";
    constexpr const char *CommonDiagMsg =
        "Following scheme may generate accesses outside of all "
        "provided RW sections in layout:\n";

    if (StrictCheck)
      snippy::fatal(Ctx, DiagPrefix, CommonDiagMsg + Twine(SchemeDump));

    snippy::warn(WarningName::MemoryAccess, Ctx, DiagPrefix,
                 CommonDiagMsg + Twine(SchemeDump));
  }
}

MemoryAccessesGenerator &MemoryScheme::getMAG() { return MAG; }

void MemoryScheme::updateMAG() {
  static auto MagCount = 0ull;
  MagCount++;
  MemoryAccessSeq Schemes;
  for (auto &Scheme :
       make_range(MA.SplitAccesses.begin(), MA.SplitAccesses.end()))
    Schemes.emplace_back(Scheme->copy());
  MAG = OwningMAG{std::move(Schemes), MagCount};
}

MemoryScheme::MemoryScheme() : MemoryScheme(MemoryAccesses()) {}

MemoryScheme::MemoryScheme(MemoryAccesses MAc, StringRef Filename)
    : MA(std::move(MAc)), MAG(MemoryAccessSeq{}, 0ull), OriginalFile(Filename) {
  updateMAG();
  updateMemoryBank();
}

std::optional<AddressInfo>
MemoryScheme::getAddressfromPlugin(size_t AccessSize, size_t Alignment,
                                   bool BurstMode, size_t InstrClassId) {
  if (!AddrPlugin.isEnabled())
    return std::nullopt;
  auto AddrOpt =
      AddrPlugin.getAddress(AccessSize, Alignment, BurstMode, InstrClassId);
  if (AddrOpt)
    return *AddrOpt;
  return std::nullopt;
}

std::optional<::AddressGlobalId> MemoryScheme::getPreselectedAddressId() {
  if (!AddrPlugin.isEnabled())
    return std::nullopt;
  return AddrPlugin.getAddressId();
}

AddressInfo MemoryScheme::randomAddress(size_t AccessSize, size_t Alignment,
                                        bool BurstMode) {
  auto ChooseGenInfo = [&](auto &&Scheme) {
    return AddressGenInfo::singleAccess(AccessSize, Alignment, BurstMode);
  };

  return std::get<AddressInfo>(randomAddressForInstructions(
      AccessSize, Alignment, ChooseGenInfo, BurstMode));
}

std::vector<AddressInfo>
MemoryScheme::randomBurstGroupAddresses(ArrayRef<AddressRestriction> ARRange,
                                        const OpcodeCache &OpcC,
                                        const SnippyTarget &SnpTgt) {
  assert(!ARRange.empty());

  std::vector<AddressInfo> Addresses;
  for (auto &AR : ARRange) {

    auto ChooseGenInfo = [&](auto &&Scheme) {
      return AddressGenInfo::singleAccess(AR.AccessSize, AR.AccessAlignment,
                                          true);
    };

    auto AI = std::get<AddressInfo>(randomAddressForInstructions(
        AR.AccessSize, AR.AccessAlignment, ChooseGenInfo,
        /*BurstMode*/ true));

    Addresses.push_back(std::move(AI));
  }

  return Addresses;
}

void MemoryScheme::fillBaseAccessesIfNeeded(
    const SectionsDescriptions &Sections, const Align &TargetAlignment) {
  if (!MA.BaseAccesses.empty())
    return;
  auto DefaultStride = TargetAlignment.value();
  std::vector<SectionDesc> V;
  std::copy_if(
      Sections.begin(), Sections.end(), std::back_inserter(V), [](auto S) {
        return S.M.W() &&
               (!S.isNamed() ||
                !SectionsDescriptions::isSpecializedSectionName(S.getName()));
      });
  std::transform(V.begin(), V.end(), std::back_inserter(MA.BaseAccesses),
                 [DefaultStride](const SectionDesc &S) {
                   MemoryAccessRange R{};
                   R.Start = S.VMA;
                   R.Size = S.Size;
                   R.Stride = DefaultStride;
                   R.FirstOffset = 0;
                   R.LastOffset = 0;
                   R.initAllowedAlignmentLCBlockOffsets();
                   return std::make_unique<MemoryAccessRange>(std::move(R));
                 });
}

} // namespace snippy

size_t yaml::SequenceTraits<snippy::SectionsDescriptions>::size(
    yaml::IO &IO, snippy::SectionsDescriptions &Sections) {
  return Sections.size();
}

snippy::SectionDesc &
yaml::SequenceTraits<snippy::SectionsDescriptions>::element(
    yaml::IO &IO, snippy::SectionsDescriptions &Sections, size_t Index) {
  if (Index >= Sections.size()) {
    Sections.push_back(snippy::SectionDesc());
    return Sections.back();
  }
  return Sections[Index];
}

} // namespace llvm
