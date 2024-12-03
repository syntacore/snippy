//===-- MemoryScheme.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Classes to handle memory accesses and scheme. Read from file.
///
/// sections:
///     - no:        1
///       VMA:       0x2000000
///       SIZE:      0x400000
///       LMA:       0x2000000
///       ACCESS:    rwx
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ImmediateHistogram.h"

#include "snippy/Plugins/MemorySchemePluginCInterface.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"

#include <unordered_set>

namespace llvm {

class MCInstrDesc;

namespace snippy {

class SnippyTarget;
class OpcodeCache;

enum class MemoryAccessMode { Range, Eviction, Addresses };

using MemAddr = uint64_t;
using MemAddresses = SmallVector<MemAddr>;

struct AddressInfo {
  // The legal address to access. It must be randomly generated accoring to any
  // allowed memory scheme.
  MemAddr Address = 0;
  // Min and max offsets for the Address. They can be useful when generating
  // strided or indirect accesses.
  int64_t MaxOffset = 0;
  int64_t MinOffset = 0;
  size_t AccessSize = 0;
  // The minimum stride that is legal for the given Address in the chosen memory
  // scheme.
  size_t MinStride = 0;
  // Example: Address = 0x5, MaxOffset =0x8, MinOffset = 0 and MinStride = 0x3.
  // Then it'll be legal (but not necessary) to use the following memory
  // accesses: 0x5, 0x8, 0xb.
  // Address + 0 * MinStride
  // |           Address + 1 * MinStride
  // |           |           Address + 2 * MinStride
  // |           |           |
  // 0x5 0x6 0x7 0x8 0x9 0xa 0xb 0xc
  // |   |   |   |   |   |   |   |
  // |<----- MaxAccessSize ----->|
};

struct MemRange {
  const MemAddr Start;
  const MemAddr End;

  explicit MemRange(MemAddr Start = 0, MemAddr End = 0)
      : Start(Start), End(End) {
    assert(Start <= End && "Illegal Memory Range");
  }

  explicit MemRange(AddressInfo AI)
      : MemRange{AI.Address, AI.Address + AI.MaxOffset + AI.AccessSize} {}

  bool interfere(const MemRange &Rhs) const {
    return !(End <= Rhs.Start || Start >= Rhs.End);
  }

  MemRange intersect(const MemRange &Rhs) const {
    if (!interfere(Rhs))
      return MemRange{};
    return MemRange{std::max(Start, Rhs.Start), std::min(End, Rhs.End)};
  }

  auto size() const { return End - Start; }
  operator bool() const { return !(End == 0 && Start == 0); }

  bool operator<(const MemRange &Rhs) const {
    return (Start == Rhs.Start) ? End < Rhs.End : Start < Rhs.Start;
  }

  bool operator==(const MemRange &Rhs) const {
    return Start == Rhs.Start && End == Rhs.End;
  }
};

class MemoryBank {
public:
  // This class stores sorted set of non-overlapping MemRanges.
  // Ranges are sorted using overloaded MemRange::operator<().
  auto begin() const { return Ranges.begin(); }
  auto end() const { return Ranges.end(); }

  //  To iterate over ranges inside MemoryBank that are
  //  overlapping with some MemRange R, lowerRange() and upperRange()
  //  could be used.
  //
  //  lowerRange() returns iterator to the first range intersecting
  //  with R. If none such range exists(ex. 2) it returns upperRange().
  //  upperRange() returns iterator past the last range intersecting
  //  with R.
  //
  //  1.
  //              R
  //       [=============]
  //       |             |
  //  [=====]  [=]    [=======]  [====]
  //     L                         U
  //
  //  2.
  //              R
  //       [=============]
  //       |             |
  //  [==] |             |   [=====] [=]
  //                           L,U
  //
  //  L = lowerRange(R);
  //  U = upperRange(R);
  //
  //

  auto lowerRange(MemRange R) const {
    auto LowerBound = Ranges.lower_bound(MemRange{R.Start, R.Start});
    if (LowerBound == Ranges.begin())
      return LowerBound;
    auto &Prev = *std::prev(LowerBound);
    return (Prev.End > R.Start) ? std::prev(LowerBound) : LowerBound;
  }

  auto upperRange(MemRange R) const {
    return Ranges.lower_bound(MemRange{R.End, R.End});
  }

  // Check if R/AI is fully contained in MemoryBank.
  bool contained(MemRange R) const;
  bool contained(AddressInfo AI) const;

  void addRange(MemRange R);
  void substractRange(MemRange R);

  MemoryBank unite(const MemoryBank &Rhs) const;
  MemoryBank intersect(const MemoryBank &Rhs) const;
  MemoryBank diff(const MemoryBank &Rhs) const;

  bool containedIn(const MemoryBank &Rhs) const {
    return diff(intersect(Rhs)).empty();
  }

  auto size() const { return Ranges.size(); }
  bool empty() const { return Ranges.empty(); }

  void clear() { Ranges.clear(); }

private:
  void mergeRanges();

  std::set<MemRange> Ranges;
};

class MemoryAccess;
using MemoryAccessSeq = SmallVector<std::unique_ptr<MemoryAccess>>;
using MemoryAccessIter = MemoryAccessSeq::iterator;

struct AddressRestriction {
  size_t AccessSize;
  size_t AccessAlignment;
  StridedImmediate ImmOffsetRange;
  std::unordered_set<unsigned> Opcodes;

  bool operator==(const AddressRestriction &AR) const {
    return AccessSize == AR.AccessSize &&
           AccessAlignment == AR.AccessAlignment &&
           ImmOffsetRange == AR.ImmOffsetRange && Opcodes == AR.Opcodes;
  }
};

// Parameters that define requirements for the randomly generated address.
struct AddressGenInfo {
  size_t AccessSize; //< Single element width
  size_t Alignment;  //< Required alignment

  // Whether burst mode is enabled. In this case some magic needs to happen.
  bool BurstMode;
  std::optional<::AddressId> PreselectedAddr = std::nullopt;

  // In order to generate addresses for multiple sequential/strided accesses
  // MinStride needs to be set. NumElements in the necessary amount of elements
  // that should fit in the resulting address range. When only a single element
  // is needed, then values of MinStride & MaxStride should be ignored
  size_t NumElements = 1; //< Initializer to have single element by default
  size_t MinStride = 0;   //< Does not matter for non-strided access

  // Some utility functions for readability
  bool isSingleElement() const { return NumElements == 1u; }

  static AddressGenInfo multiAccess(size_t AccessSize, size_t Alignment,
                                    bool Burst, size_t NumElements,
                                    size_t MinStride) {
    assert(NumElements >= 1 && "NumElements can't be 0");
    assert(isPowerOf2_64(Alignment) && "Alignment should be a power of 2");

    AddressGenInfo Params;
    Params.AccessSize = AccessSize;
    Params.Alignment = Alignment;
    Params.BurstMode = Burst;
    Params.NumElements = NumElements;
    Params.MinStride = MinStride;
    return Params;
  }

  static AddressGenInfo singleAccess(size_t AccessSize, size_t Alignment,
                                     bool Burst) {
    return multiAccess(AccessSize, Alignment, Burst, /* Num Elements */ 1,
                       /* No Stride */ 0);
  }
};

class MemoryAccess {
public:
  MemoryAccess(MemoryAccessMode MAM) : Mode(MAM) {}

  MemoryAccessMode getMode() const { return Mode; }

  virtual AddressInfo randomAddress(const AddressGenInfo &Info) = 0;
  virtual MemAddr getLowerBound() const = 0;
  virtual MemoryBank getPossibleAddresses() const = 0;
  virtual MemoryAccessSeq split(const MemoryBank &MB) const = 0;
  virtual std::unique_ptr<MemoryAccess> copy() const = 0;
  virtual bool isLegal(const AddressGenInfo &Info) const = 0;
  virtual ~MemoryAccess() {}

public:
  double Weight = 1.0;

private:
  MemoryAccessMode Mode;
};

struct SectionDesc;

struct MemoryAccessRange final : MemoryAccess {
  MemAddr Start = 0;
  size_t Size = 0;
  size_t Stride = 1;
  size_t FirstOffset = 0;
  size_t LastOffset = 0;
  std::optional<size_t> AccessSize = std::nullopt;

private:
  // When alignment must be accounted for, offsets that are aligned in one
  // Stride-wide block might not be aligned in the next one. However, they will
  // be aligned again if we add the least common multiple of Stride and
  // Alignment (LCStride).
  // To keep address sampling simple, instead of selecting an offset in a
  // Stride-wide block, it's possible to precompute a list of all the offsets
  // that are both aligned and allowed by the current memory scheme in an
  // LCStride-wide block, and then select one of these offsets instead.
  // This assumes that Stride might not be a power of 2, but is still rather
  // small.
  // NOTE: mutable here because we are caching calculated offsets. Won't work
  // well if we'll want to parallel it.
  mutable std::array<std::optional<SmallVector<size_t>>, 4>
      AlignmentAllowedLCBlockOffsets;

public:
  MemoryAccessRange() : MemoryAccess(MemoryAccessMode::Range) {}

  MemoryAccessRange(MemAddr StartAddr, size_t Size, unsigned Stride,
                    unsigned FirstOffset, unsigned LastOffset)
      : MemoryAccess(MemoryAccessMode::Range), Start(StartAddr), Size(Size),
        Stride(Stride), FirstOffset(FirstOffset), LastOffset(LastOffset) {}

  MemoryAccessRange(const SectionDesc &S, unsigned Alignment);

  static bool classof(const MemoryAccess *Access) {
    return Access->getMode() == MemoryAccessMode::Range;
  }

  MemoryBank getPossibleAddresses() const override;
  MemoryAccessSeq split(const MemoryBank &MB) const override;

  bool isLegal(const AddressGenInfo &Info) const override;
  AddressInfo randomAddress(const AddressGenInfo &Info) override;

  std::unique_ptr<MemoryAccess> copy() const override {
    return std::make_unique<MemoryAccessRange>(*this);
  }

  MemAddr getLowerBound() const override { return Start; }

private:
  // Get the least common multiple of alignment and stride. This is necessary to
  // respect both the access alignment and the stride specified by the user.
  size_t getLCStride(size_t Alignment) const {
    return std::lcm(Stride, Alignment);
  }

  void getAllowedOffsetsImpl(size_t Alignment,
                             SmallVectorImpl<size_t> &Out) const;

  ArrayRef<size_t> getAllowedOffsets(size_t Alignment) const;

  // Find the last LCStride-wide block that can fit AccessSize-wide element with
  // the specified alignment.
  size_t getMaxLCBlock(size_t Alignment, size_t AccessSize,
                       ArrayRef<size_t> AllowedLCBlockOffsets) const {
    auto LCStride = getLCStride(Alignment);
    assert(!AllowedLCBlockOffsets.empty());
    auto MaxOffset = Size - AccessSize;
    // Can select block if LCBlock * LCStride + AllowedLCBlockOffsets.front() <=
    // MaxOffset
    auto MaxLCBlock = (MaxOffset - AllowedLCBlockOffsets.front()) / LCStride;
    return MaxLCBlock;
  }
};

struct MemoryAccessEviction final : MemoryAccess {
  MemAddr Mask = 0;
  MemAddr Fixed = 0;
  std::optional<size_t> AccessSize = std::nullopt;

  MemoryAccessEviction() : MemoryAccess(MemoryAccessMode::Eviction) {}

  static bool classof(const MemoryAccess *Access) {
    return Access->getMode() == MemoryAccessMode::Eviction;
  }

  AddressInfo randomAddress(const AddressGenInfo &Info) override;
  MemoryBank getPossibleAddresses() const override;
  MemoryAccessSeq split(const MemoryBank &MB) const override;

  std::unique_ptr<MemoryAccess> copy() const override {
    return std::make_unique<MemoryAccessEviction>(*this);
  }

  bool isLegal(const AddressGenInfo &Info) const override {
    auto Alignment = Info.Alignment;
    assert(isPowerOf2_64(Alignment));
    if ((Alignment - 1) & Fixed)
      // If alignment requires some Fixed bits to be 0, then this alignment is
      // not supported
      return false;

    // FIXME: Maybe support strided accesses with evictions? This would require
    // finding the longest consequtive run with some bit manipulation magic and
    // somehow choosing one if there are many non-fixed runs.
    if (!Info.isSingleElement())
      return false;

    if (!AccessSize)
      return Info.AccessSize <= 8;
    return Info.AccessSize <= *AccessSize;
  }

  MemAddr getLowerBound() const override { return Fixed; }
};

struct AccessAddress {
  MemAddr Addr;
  size_t AccessSize;
};

using AddressSeq = std::vector<AccessAddress>;

struct MemoryAccessAddresses final : MemoryAccess {
  std::optional<size_t> NextAddressIdx;
  AddressSeq Addresses;
  std::optional<size_t> NextBurstIdx;
  std::vector<AddressInfo> Burst;

  MemoryAccessAddresses() : MemoryAccess(MemoryAccessMode::Addresses) {}

  static bool classof(const MemoryAccess *Access) {
    return Access->getMode() == MemoryAccessMode::Addresses;
  }

  AddressInfo randomAddress(const AddressGenInfo &Info) override;
  MemoryBank getPossibleAddresses() const override;
  MemoryAccessSeq split(const MemoryBank &MB) const override;
  MemoryAccessAddresses splitPlainAccesses(const MemoryBank &MB) const;
  MemoryAccessAddresses splitBurstAccesses(const MemoryBank &MB) const;

  std::unique_ptr<MemoryAccess> copy() const override {
    return std::make_unique<MemoryAccessAddresses>(*this);
  }

  AddressInfo randomAddressForPlainAccess(
      size_t AccessSize, size_t Alignment,
      std::optional<::AddressId> PreselectedAddr = std::nullopt);

  AddressInfo randomAddressForBurstAccess(
      size_t AccessSize, size_t Alignment,
      std::optional<::AddressId> PreselectedAddr = std::nullopt);

  bool isLegal(const AddressGenInfo &Info) const override;

  // FIXME: any reason to keep `getlowerbound` as a virtual function?
  MemAddr getLowerBound() const override {
    snippy::fatal(
        "getLowerBound for 'Addresses' memory scheme is not implemented");
  }
};

struct MemoryAccessesGroup {
  double Weight = 1.0;
  MemoryAccessSeq BaseGroupAccesses;
};

using MemoryAccessesGroupSeq = SmallVector<MemoryAccessesGroup>;

struct SectionsDescriptions;

enum class Acc {
  R = 1,
  W = 2,
  X = 4,
};

struct AccMask {
  int M = 0;
  AccMask() = default;
  AccMask(int M) : M(M) {}
  AccMask(Acc T) : M(static_cast<int>(T)) {}

  static Expected<AccMask> fromString(StringRef Mode) {
    int M = 0;

    for (auto C : Mode)
      switch (tolower(C)) {
      case 'r':
        M |= static_cast<int>(Acc::R);
        break;
      case 'w':
        M |= static_cast<int>(Acc::W);
        break;
      case 'x':
        M |= static_cast<int>(Acc::X);
        break;
      default:
        return createStringError(
            std::make_error_code(std::errc::invalid_argument),
            formatv("Invalid access attribute in config: {0} is not allowed. "
                    "Acceptable ones: r, w, x.",
                    C));
      }

    return M;
  }

  AccMask(StringRef Mode) {
    if (auto E = fromString(Mode).moveInto(*this))
      snippy::fatal(std::move(E));
  }

  AccMask(const char *Mode) : AccMask(StringRef(Mode)) {}

  operator int() const { return M; }

  AccMask &operator|=(Acc T) {
    M |= static_cast<int>(T);
    return *this;
  }

  AccMask operator|(Acc T) const {
    AccMask Tmp = M;
    Tmp |= T;
    return Tmp;
  }

  AccMask &operator&=(const Acc T) {
    M &= static_cast<int>(T);
    return *this;
  }

  AccMask operator&(const Acc T) const {
    AccMask Tmp = M;
    Tmp &= T;
    return Tmp;
  }

  bool operator==(const Acc &T) const { return M == static_cast<int>(T); }

  bool R() const { return (M & static_cast<int>(Acc::R)) != 0; }
  bool W() const { return (M & static_cast<int>(Acc::W)) != 0; }
  bool X() const { return (M & static_cast<int>(Acc::X)) != 0; }

  void dump(llvm::raw_ostream &Stream) const {
    if (R())
      Stream << "r";
    if (W())
      Stream << "w";
    if (X())
      Stream << "x";
  }
};

struct SectionDesc {
  std::variant<int, std::string> ID;
  size_t VMA;
  size_t Size;
  size_t LMA;
  AccMask M;
  std::optional<std::string> Phdr;

  SectionDesc(int Num = 0, size_t VMAIn = 0, size_t SizeIn = 0,
              size_t LMAIn = 0, AccMask Mask = "rwx",
              std::optional<std::string> Phdr = std::nullopt)
      : ID(Num), VMA(VMAIn), Size(SizeIn), LMA(LMAIn), M(Mask),
        Phdr{std::move(Phdr)} {}

  SectionDesc(StringRef Name, size_t VMAIn = 0, size_t SizeIn = 0,
              size_t LMAIn = 0, AccMask Mask = "rwx",
              std::optional<std::string> Phdr = std::nullopt)
      : ID(std::string(Name)), VMA(VMAIn), Size(SizeIn), LMA(LMAIn), M(Mask),
        Phdr{std::move(Phdr)} {}

  SectionDesc(std::variant<int, std::string> ID, size_t VMAIn = 0,
              size_t SizeIn = 0, size_t LMAIn = 0, AccMask Mask = "rwx",
              std::optional<std::string> Phdr = std::nullopt)
      : ID(ID), VMA(VMAIn), Size(SizeIn), LMA(LMAIn), M(Mask),
        Phdr{std::move(Phdr)} {}

  bool interfere(SectionDesc const &another) const {
    if (VMA >= another.VMA + another.Size || VMA + Size <= another.VMA)
      return false;
    else
      return true;
  }

  bool hasPhdr() const { return Phdr.has_value(); }
  bool isNamed() const { return std::holds_alternative<std::string>(ID); }

  StringRef getName() const {
    assert(isNamed() && "cannot get section name: section ID is int");
    return std::get<std::string>(ID);
  }

  int getNumber() const {
    assert(!isNamed() && "cannot get section number: this is named section");
    return std::get<int>(ID);
  }

  std::string getIDString() const {
    if (isNamed())
      return std::string(getName());
    else
      return std::to_string(getNumber());
  }

  StringRef getPhdr() const {
    assert(hasPhdr() && "Section does not have a phdr.");
    return Phdr.value();
  }

  bool hasAccess(Acc Type) const { return (M & Type) != 0; }
  size_t getSize() const { return Size; }
};

inline bool operator==(const SectionDesc &Lhs, const SectionDesc &Rhs) {
  return Lhs.VMA == Rhs.VMA && Lhs.LMA == Rhs.LMA && Lhs.M.M == Rhs.M.M &&
         Lhs.Size == Rhs.Size;
}

struct SectionsDescriptions : private std::vector<SectionDesc> {
  using vector::at;
  using vector::back;
  using vector::begin;
  using vector::empty;
  using vector::end;
  using vector::front;
  using vector::operator[];
  using vector::pop_back;
  using vector::push_back;
  using vector::rbegin;
  using vector::rend;
  using vector::resize;
  using vector::size;

  auto &lastRWSection() const {
    assert(std::any_of(begin(), end(),
                       [](auto &S) { return S.M.R() && S.M.W() && !S.M.X(); }));
    return *std::find_if(rbegin(), rend(), [](auto &S) {
      return S.M.R() && S.M.W() && !S.M.X();
    });
  }

  // Range of all RW sections excluding
  // specialized ones(e.g. stack and selfcheck).
  auto generalRWSections() const {
    return make_filter_range(*this, [](auto &Sec) {
      auto Access = Sec.M;
      return Access.R() && Access.W() &&
             (!Sec.isNamed() || !isSpecializedSectionName(Sec.getName()));
    });
  };
  static constexpr const char *StackSectionName = "stack";
  static constexpr const StringRef UtilitySectionName = "utility";
  static constexpr const char *SelfcheckSectionName = "selfcheck";

  // Default size for implicitly generated sections
  static constexpr const auto ImplicitSectionSize = 0x1000;

  static bool isSpecializedSectionName(StringRef Name) {
    return Name == StackSectionName || Name == SelfcheckSectionName;
  }
  bool hasSection(StringRef Name) const {
    return std::any_of(begin(), end(), [Name](auto &S) {
      return S.isNamed() && Name == S.getName();
    });
  }

  auto &getSection(StringRef Name) const {
    assert(hasSection(Name) && "No section with given name");
    auto Found = std::find_if(begin(), end(), [Name](auto &S) {
      return S.isNamed() && Name == S.getName();
    });
    return *Found;
  }

  auto &getSection(StringRef Name) {
    assert(hasSection(Name) && "No section with given name");
    auto Found = std::find_if(begin(), end(), [Name](auto &S) {
      return S.isNamed() && Name == S.getName();
    });
    return *Found;
  }

  auto getSectionsSize(Acc AccType) const {
    return std::accumulate(
        begin(), end(), 0ull,
        [AccType](const size_t CurrentSize, const auto &Section) {
          if (Section.hasAccess(AccType))
            return CurrentSize + Section.getSize();
          return CurrentSize;
        });
  }
};

template <typename SecIt>
void diagnoseXSections(LLVMContext &Ctx, SecIt SectionsStart, SecIt SectionsFin,
                       size_t Alignment) {
  // TODO: assert for 1 rx section?
  std::vector<SectionDesc> ExecSections;
  std::copy_if(SectionsStart, SectionsFin, std::back_inserter(ExecSections),
               [](const auto &CurSection) { return CurSection.M.X(); });

  if (ExecSections.empty()) {
    snippy::fatal(Ctx, "Incorrect list of sections",
                  "there are no X-accessible sections.");
    return;
  }
  for (auto &&ExecSection : ExecSections) {
    auto Access = ExecSection.M;

    if (!Access.R()) {
      snippy::warn(
          WarningName::MemoryAccess, Ctx, "Incorrect section",
          "The executable section " + Twine(ExecSection.getIDString()) +
              " has not R access mode. Implicitly consider it like RX...");
    }
    if (Access.W()) {
      snippy::warn(
          WarningName::MemoryAccess, Ctx, "Incorrect section",
          "The executable section " + Twine(ExecSection.getIDString()) +
              " has also W access mode. Snippy does not support SMC for now");
    }

    if (ExecSection.VMA % Alignment)
      snippy::fatal(Ctx, "Incorrect section",
                    "The executable section '" +
                        Twine(ExecSection.getIDString()) +
                        "' must be aligned to " + Twine(Alignment) +
                        " according to specified config");
  }
}

class MemoryScheme {
public:
  MemoryAccessSeq BaseAccesses;
  MemoryBank Restricted;

public:
  std::optional<std::string>
  validateSchemes(LLVMContext &Ctx, const SectionsDescriptions &Sections) const;
};

template <typename AccessesRange>
std::optional<MemAddr> getFirstNeededMemoryAccessBound(AccessesRange &&Accesses,
                                                       MemoryAccessMode Mode) {
  auto *Pos =
      llvm::find_if(Accesses, [Mode](const std::unique_ptr<MemoryAccess> &MA) {
        return MA->getMode() == Mode;
      });
  if (Pos == Accesses.end())
    return {};
  return (*Pos)->getLowerBound();
}
} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::MemoryAccessRange);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::MemoryAccessEviction);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::MemoryAccessAddresses);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::MemoryBank);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::MemoryAccessesGroup);

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::MemoryAccessRange,
                                     /* not flow */ false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::MemoryAccessEviction,
                                     /* not flow */ false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::MemoryAccessAddresses,
                                     /* not flow */ false);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::MemoryAccessesGroup,
                                     /* not flow */ false);

LLVM_SNIPPY_YAML_DECLARE_SCALAR_TRAITS_NG(snippy::AccMask);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::SectionDesc);
LLVM_SNIPPY_YAML_DECLARE_SEQUENCE_TRAITS(snippy::SectionsDescriptions,
                                         snippy::SectionDesc);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::MemoryScheme);

} // namespace llvm
