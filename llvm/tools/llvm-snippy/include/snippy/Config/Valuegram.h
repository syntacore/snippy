//===-- Valuegram.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {
namespace snippy {

class ValuegramEntry;

namespace detail {

// NOTE: Map and sequence mappers should be separate types that reference
// the underlying polymorphic entry, but also implement either only mapping
// traits or sequence traits.

struct ValuegramEntryMapMapper final {
  ValuegramEntry &TheEntry;
};

struct ValuegramEntryScalarMapper final {};

} // namespace detail

enum class InputFormat { Regular, Unsupported };

struct APIntWithSign {
  APInt Value;
  bool IsSigned;
  static Expected<APInt> parseAPInt(StringRef StrView, bool HasNegativeSign,
                                    unsigned Radix, StringRef OriginalStr);
  static Error reportError(Twine Msg);
};

struct FormattedAPIntWithSign {
  APIntWithSign Number;
  InputFormat Format = InputFormat::Regular;
  static Expected<FormattedAPIntWithSign> fromString(StringRef StrView);
  Expected<std::string> toString() const;
  const APInt &getVal() const & { return Number.Value; }
  bool isSigned() const { return Number.IsSigned; }
};

// Valuegram is an ordered collection of arbitrary weighted types.
// Typically this should be used to represent an extended histogram types
// with arbitrary mappings like:
//
// valuegram.yaml:
// - [bitpattern, 1.0]
// - type: bitpattern
//   weight: 1.0
//
// For back-compat reasons valuegram format is a superset of YAMLHistogram,
// but is less generic.

class IValuegramEntry {
public:
  enum class EntryKind {
    MapEntry, //< Internal, represents any entry encoded as a YAMP mapping.
    BitPattern,
    Uniform,
    BitValue,
    BitRange,
    HelperSentinel, //< Should not be used directly. Used to indicate
                    // past-the-end enum value. Must always be the last.
  };

public:
  virtual ~IValuegramEntry() = default;
  virtual EntryKind getKind() const = 0;
  virtual std::unique_ptr<IValuegramEntry> clone() = 0;

  double Weight = 1.0;
};

StringRef toString(IValuegramEntry::EntryKind Kind);

class ValuegramPattern {
public:
  enum class PatternKind {
    BitPattern,
    Uniform,
  };

private:
  static std::optional<PatternKind>
  getPatternKindOrNone(IValuegramEntry::EntryKind EntryKind) {
    switch (EntryKind) {
    case IValuegramEntry::EntryKind::BitPattern:
      return PatternKind::BitPattern;
    case IValuegramEntry::EntryKind::Uniform:
      return PatternKind::Uniform;
    default:
      return std::nullopt;
    }
  }

public:
  static bool isPattern(IValuegramEntry::EntryKind EntryKind) {
    return getPatternKindOrNone(EntryKind).has_value();
  }

  static Expected<ValuegramPattern>
  create(IValuegramEntry::EntryKind EntryKind) {
    if (auto Pattern = getPatternKindOrNone(EntryKind))
      return ValuegramPattern(*Pattern);
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        Twine(toString(EntryKind)).concat(" is not a pattern kind"));
  }

  constexpr explicit ValuegramPattern(PatternKind Kind) : TheKind(Kind) {}
  constexpr operator PatternKind() const noexcept { return TheKind; }
  constexpr operator IValuegramEntry::EntryKind() const noexcept {
    switch (TheKind) {
    case PatternKind::BitPattern:
      return IValuegramEntry::EntryKind::BitPattern;
    case PatternKind::Uniform:
      return IValuegramEntry::EntryKind::Uniform;
    }
  }

  static const ValuegramPattern BitPattern;
  static const ValuegramPattern Uniform;

  PatternKind TheKind;
};

constexpr inline ValuegramPattern ValuegramPattern::BitPattern =
    ValuegramPattern(ValuegramPattern::PatternKind::BitPattern);
constexpr inline ValuegramPattern ValuegramPattern::Uniform =
    ValuegramPattern(ValuegramPattern::PatternKind::Uniform);

// NOTE: Ideally we would have diamond inheritance of interfaces for Valuegram
// entries. All pattern entries are a sequence and a mapping at the same time.
// But LLVM does not allow multiple inheritance that is compatible with
// LLVM-Style RTTI. Because of this this code opts to put all interface
// functions in the IValuegramEntry class, but makes them illegal to call
// from incompatible derived classes.
class IValuegramMapEntry : public IValuegramEntry {
protected:
  virtual void mapYamlImpl(yaml::IO &Io) {}

public:
  static bool classof(const IValuegramEntry *Entry) {
    auto Kind = Entry->getKind();
    return (Kind > EntryKind::MapEntry && Kind < EntryKind::HelperSentinel);
  }

  void mapYaml(yaml::IO &Io);
  virtual std::string validate(yaml::IO &Io) const { return ""; };
};

struct ValuegramBitpatternEntry final : public IValuegramMapEntry {
  ValuegramBitpatternEntry() = default;
  EntryKind getKind() const override { return EntryKind::BitPattern; }
  static bool classof(const IValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::BitPattern;
  }
  std::unique_ptr<IValuegramEntry> clone() override {
    return std::make_unique<ValuegramBitpatternEntry>(*this);
  }
};

struct ValuegramUniformEntry final : public IValuegramMapEntry {
  ValuegramUniformEntry() = default;
  EntryKind getKind() const override { return EntryKind::Uniform; }
  static bool classof(const IValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::Uniform;
  }
  std::unique_ptr<IValuegramEntry> clone() override {
    return std::make_unique<ValuegramUniformEntry>(*this);
  }
};

struct ValuegramBitValueEntry final : public IValuegramMapEntry {
  FormattedAPIntWithSign ValWithSign;

  ValuegramBitValueEntry() = default;
  ValuegramBitValueEntry(FormattedAPIntWithSign Val)
      : ValWithSign{std::move(Val)} {}
  EntryKind getKind() const override { return EntryKind::BitValue; }

  bool isSigned() const { return ValWithSign.Number.IsSigned; };
  InputFormat getFormat() const { return ValWithSign.Format; }

  const APInt &getVal() const & { return ValWithSign.Number.Value; }

  static bool classof(const IValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::BitValue;
  }

  std::unique_ptr<IValuegramEntry> clone() override {
    return std::make_unique<ValuegramBitValueEntry>(*this);
  }

protected:
  void mapYamlImpl(yaml::IO &Io) override;
};

struct ValuegramBitRangeEntry final : public IValuegramMapEntry {
  // Values are always treated as raw bits, range is unsigned regardless
  // and it's illegal to deserialize signed values.

  APInt Min;
  APInt Max;

  ValuegramBitRangeEntry() = default;
  ValuegramBitRangeEntry(APInt MinParam, APInt MaxParam)
      : Min(std::move(MinParam)), Max(std::move(MaxParam)) {}

  EntryKind getKind() const override { return EntryKind::BitRange; }

  static bool classof(const IValuegramEntry *Entry) {
    assert(Entry);
    auto Kind = Entry->getKind();
    return Kind == EntryKind::BitRange;
  }

  std::unique_ptr<IValuegramEntry> clone() override {
    return std::make_unique<ValuegramBitRangeEntry>(*this);
  }

protected:
  void mapYamlImpl(yaml::IO &Io) override;
};

Expected<std::unique_ptr<IValuegramEntry>>
createValuegramSequenceEntry(IValuegramEntry::EntryKind Kind);

struct RegisterClassHistogram;
class Valuegram;

class ValuegramEntry final {
private:
  // NOTE: Due to the fact that we are using YAMLTraits from LLVM
  // this type has to be default constructible. Otherwise it would be impossible
  // to deserialize it.
  ValuegramEntry() = default;

  // Internal nodes of the hierarchy shall not be passed here.
  // It's private API and user code should create ValuegramEntry only
  // through the failable constructor.
  ValuegramEntry(std::unique_ptr<IValuegramEntry> UncheckedEntry)
      : UnderlyingEntry{std::move(UncheckedEntry)} {}

  friend struct yaml::PolymorphicTraits<snippy::ValuegramEntry>;
  friend struct yaml::MappingTraits<snippy::RegisterClassHistogram>;
  friend struct yaml::SequenceTraits<snippy::Valuegram>;
  friend struct yaml::MappingTraits<
      const snippy::detail::ValuegramEntryMapMapper>;
  std::unique_ptr<IValuegramEntry> &getOwned() { return UnderlyingEntry; }

  // NOTE: These methods are only necessary for the validation phase of YAML
  // parser. Valid YAML will result only in non-default constructed
  // ValuegramEntry with values.

public:
  IValuegramEntry *getOrNull() { return UnderlyingEntry.get(); }
  const IValuegramEntry *getOrNull() const { return UnderlyingEntry.get(); }

  bool hasValue() const { return UnderlyingEntry.get(); }
  operator bool() const { return hasValue(); }

  using EntryKind = IValuegramEntry::EntryKind;

  ValuegramEntry(const ValuegramEntry &Other)
      : UnderlyingEntry(Other.UnderlyingEntry ? Other.UnderlyingEntry->clone()
                                              : nullptr) {}
  ValuegramEntry(ValuegramEntry &&Other) = default;
  ValuegramEntry &operator=(ValuegramEntry &&Other) = default;
  ValuegramEntry &operator=(const ValuegramEntry &Other) {
    return *this = ValuegramEntry{Other};
  }

  static Expected<ValuegramEntry> create(EntryKind Kind);
  static Expected<ValuegramEntry>
  create(std::unique_ptr<IValuegramEntry> OwningEntry);

  IValuegramEntry &get() {
    assert(UnderlyingEntry);
    return *UnderlyingEntry;
  }

  const IValuegramEntry &get() const {
    assert(UnderlyingEntry);
    return *UnderlyingEntry;
  }

  double &getWeight() {
    assert(UnderlyingEntry);
    return UnderlyingEntry->Weight;
  }

  double getWeight() const {
    assert(UnderlyingEntry);
    return UnderlyingEntry->Weight;
  }

  EntryKind getKind() const {
    assert(UnderlyingEntry);
    return UnderlyingEntry->getKind();
  }

private:
  std::unique_ptr<IValuegramEntry> UnderlyingEntry;
};

class Valuegram final : private std::vector<ValuegramEntry> {
  using BaseType = std::vector<ValuegramEntry>;

public:
  using iterator = BaseType::iterator;
  using const_iterator = BaseType::const_iterator;

  using BaseType::at;
  using BaseType::size;
  using BaseType::operator[];
  using BaseType::begin;
  using BaseType::cbegin;
  using BaseType::cend;
  using BaseType::crbegin;
  using BaseType::crend;
  using BaseType::empty;
  using BaseType::end;
  using BaseType::rbegin;
  using BaseType::rend;
  using BaseType::resize;

  auto weights() const {
    return map_range(*this, [&](auto &&Entry) { return Entry.getWeight(); });
  }

  auto weights() {
    return map_range(*this, [&](auto &&Entry) { return Entry.getWeight(); });
  }

  auto weights_begin() const { return weights().begin(); }
  auto weights_end() const { return weights().end(); }
  auto weights_begin() { return weights().begin(); }
  auto weights_end() { return weights().end(); }
};

namespace detail {

struct HistogramSubsetForValuegram final {};

} // namespace detail

// NOTE: These traits are not fully implemented, but can be expanded to
// support YAMLHistogramIO.
LLVM_SNIPPY_YAML_DECLARE_HISTOGRAM_TRAITS(std::unique_ptr<IValuegramEntry>,
                                          detail::HistogramSubsetForValuegram);

} // namespace snippy

template <> struct llvm::yaml::SequenceTraits<snippy::Valuegram, void> {
  static size_t size(IO &Io, snippy::Valuegram &List);
  static snippy::ValuegramEntry &element(IO &Io, snippy::Valuegram &List,
                                         size_t Index);
};

// NOTE: We do not use LLVM_SNIPPY_YAML_DECLARE_.... here, because
// specialization needs to be for const-qualified types and using them
// directly would lead to a lot of double-const warnings.

template <>
struct llvm::yaml::ScalarTraits<
    const snippy::detail::ValuegramEntryScalarMapper, void> {
  [[noreturn]] static void reportUnreachable() {
    llvm_unreachable("ValuegramEntryScalarMapper should never be used");
  }
  static void output(const snippy::detail::ValuegramEntryScalarMapper &, void *,
                     raw_ostream &) {
    reportUnreachable();
  }
  static StringRef
  input(StringRef Scalar, void *,
        const snippy::detail::ValuegramEntryScalarMapper &Value) {
    reportUnreachable();
  }
  static QuotingType mustQuote(StringRef) { reportUnreachable(); }
};

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    const snippy::detail::ValuegramEntryMapMapper);

template <> struct llvm::yaml::PolymorphicTraits<snippy::ValuegramEntry> {
  static NodeKind getKind(const snippy::ValuegramEntry &);

  // WARNING: Due to the fact that yamlize expects an lvalue reference we
  // can't pass a wrapper type, without storing it somewhere. One sane
  // solution is to return a const object which stores a mutable reference to
  // the ValuegramEntry. This might be somewhat brittle, but looks fine from
  // inspecting the implementation:

  // template <typename T>
  // std::enable_if_t<has_PolymorphicTraits<T>::value, void>
  // yamlize(IO &io, T &Val, bool, EmptyContext &Ctx) {
  // ...
  //   case NodeKind::Scalar:
  // return yamlize(io, PolymorphicTraits<T>::getAsScalar(Val), true, Ctx);

  // Don't blame this code but rather YAMLTraits internals for this wonderful
  // kludge.

  // NOTE: Return by const value is intentional and ensures that T& will bind to
  // a temporary as a reference to const.

  static const snippy::detail::ValuegramEntryScalarMapper
  getAsScalar(snippy::ValuegramEntry &);

  static const snippy::detail::ValuegramEntryMapMapper
  getAsMap(snippy::ValuegramEntry &);

  // NOTE: Sequence entries require a special hack, due to the fact that
  // custom normalization is required. It reuses the same machinery as
  // the YAMLHistogram mechanism.

  static std::unique_ptr<snippy::IValuegramEntry> &
  getAsSequence(snippy::ValuegramEntry &);
};

LLVM_SNIPPY_YAML_DECLARE_SCALAR_TRAITS_NG(snippy::FormattedAPIntWithSign);

LLVM_SNIPPY_YAML_DECLARE_IS_HISTOGRAM_DENORM_ENTRY(
    std::unique_ptr<snippy::IValuegramEntry>)

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(
    snippy::IValuegramEntry::EntryKind);

} // namespace llvm
