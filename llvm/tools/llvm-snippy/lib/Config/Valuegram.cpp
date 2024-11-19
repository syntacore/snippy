//===-- Valuegram.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Valuegram.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {

using EntryKind = snippy::IValuegramEntry::EntryKind;

void yaml::ScalarEnumerationTraits<snippy::IValuegramEntry::EntryKind>::
    enumeration(yaml::IO &Io, snippy::IValuegramEntry::EntryKind &Kind) {
  using EntryKind = snippy::IValuegramEntry::EntryKind;
  Io.enumCase(Kind, "bitpattern", EntryKind::BitPattern);
  Io.enumCase(Kind, "uniform", EntryKind::Uniform);
  Io.enumCase(Kind, "bitvalue", EntryKind::BitValue);
  Io.enumCase(Kind, "bitrange", EntryKind::BitRange);
}

void yaml::MappingTraits<const snippy::detail::ValuegramEntryMapMapper>::
    mapping(yaml::IO &Io,
            const snippy::detail::ValuegramEntryMapMapper &Mapper) {
  using EntryKind = snippy::IValuegramEntry::EntryKind;
  auto &EntryRef = Mapper.TheEntry;

  const auto Kind = [&]() {
    EntryKind Kind =
        Io.outputting() ? EntryRef.getKind() : EntryKind::HelperSentinel;
    Io.mapRequired("type", Kind);
    return Kind;
  }();

  if (!Io.outputting())
    if (Error E = snippy::ValuegramEntry::create(Kind).moveInto(EntryRef)) {
      // NOTE: Need to consume Error to make error handling happy. We don't
      // actually need this message to give a nice diagnostic.
      snippy::burrowError(std::move(E));
      return;
    }

  auto &MapEntry = cast<snippy::IValuegramMapEntry>(EntryRef.get());
  MapEntry.mapYaml(Io);
}

std::string
yaml::MappingTraits<const snippy::detail::ValuegramEntryMapMapper>::validate(
    yaml::IO &Io, const snippy::detail::ValuegramEntryMapMapper &Mapper) {
  if (!Mapper.TheEntry.hasValue())
    return "";
  return cast<snippy::IValuegramMapEntry>(Mapper.TheEntry.get()).validate(Io);
}

yaml::NodeKind yaml::PolymorphicTraits<snippy::ValuegramEntry>::getKind(
    const snippy::ValuegramEntry &Entry) {
  return NodeKind::Map;
}

const snippy::detail::ValuegramEntryScalarMapper
yaml::PolymorphicTraits<snippy::ValuegramEntry>::getAsScalar(
    snippy::ValuegramEntry &Entry) {
  // NOTE: We can't diagnose properly via yaml::IO, since we dont' have it in
  // polymorphic traits. It could be worked around by saving the error inside
  // ValuegramEntry, but that would utterly break SRP and be too much hassle.
  LLVMContext Ctx;
  snippy::fatal(
      Ctx, "Invalid valuegram",
      createStringError((std::make_error_code(std::errc::invalid_argument)),
                        "Valuegram entry of scalar type is not supported"));
  // Just in case snippy::fatal does not actually call std::exit. TODO: Mark
  // snippy::fatal as [[noreturn]] to silence compiler warnings in such cases.
  llvm_unreachable("Valuegram entry of scalar type is not supported");
}

const snippy::detail::ValuegramEntryMapMapper
yaml::PolymorphicTraits<snippy::ValuegramEntry>::getAsMap(
    snippy::ValuegramEntry &Entry) {
  return snippy::detail::ValuegramEntryMapMapper{Entry};
}

std::unique_ptr<snippy::IValuegramEntry> &
yaml::PolymorphicTraits<snippy::ValuegramEntry>::getAsSequence(
    snippy::ValuegramEntry &Entry) {
  return Entry.getOwned();
}

LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(
    std::unique_ptr<snippy::IValuegramEntry>)

size_t yaml::SequenceTraits<snippy::Valuegram>::size(IO &Io,
                                                     snippy::Valuegram &List) {
  return List.size();
}

snippy::ValuegramEntry &yaml::SequenceTraits<snippy::Valuegram>::element(
    IO &Io, snippy::Valuegram &List, size_t Index) {
  if (auto TargetSize = Index + 1; List.size() < TargetSize)
    // Explicitly default-constructing ValuegramEntry because
    // default ctor is private and should only be invoked by YAML related code
    // without error messages .
    List.resize(TargetSize, snippy::ValuegramEntry{});
  return List[Index];
}

StringRef
yaml::ScalarTraits<snippy::APIntWithSign>::input(StringRef Input, void *,
                                                 snippy::APIntWithSign &Val) {
  if (auto E = Val.fromString(Input).moveInto(Val)) {
    // NOTE: Unfortunately here we can't diagnose the exact error message,
    // without reimplementing fromString logic completely. ::input method
    // return a non-owning StringRef, which can't take the error message.
    snippy::burrowError(std::move(E));
    return "Invalid number";
  }
  return {};
}

void yaml::ScalarTraits<snippy::APIntWithSign>::output(
    const snippy::APIntWithSign &ValWithSign, void *, raw_ostream &OS) {
  SmallString<16> SmallStr;
  ValWithSign.Value.toString(SmallStr, /*Radix=*/16, ValWithSign.IsSigned,
                             /*formatAsCLiteral=*/true,
                             /*UpperCase=*/false);
  OS << SmallStr;
}

yaml::QuotingType
yaml::ScalarTraits<snippy::APIntWithSign>::mustQuote(StringRef) {
  return QuotingType::None;
}

namespace snippy {

Error APIntWithSign::reportError(Twine Msg) {
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           Msg);
}

Expected<APInt> APIntWithSign::parseAPInt(StringRef StrView,
                                          bool HasNegativeSign, unsigned Radix,
                                          StringRef OriginalStr) {
  if (HasNegativeSign && Radix != 10)
    return reportError(
        Twine("Value '")
            .concat(OriginalStr)
            .concat("' is negative, but the radix is not equal to 10"));

  APInt Value(64 /* numBits */, 0 /* val */);
  auto ParseFailed = StrView.getAsInteger(Radix, Value);
  if (ParseFailed)
    return reportError(Twine("Value '")
                           .concat(OriginalStr)
                           .concat("' is not a valid integer"));

  if (HasNegativeSign) {
    Value = Value.zextOrTrunc(Value.getSignificantBits());
    Value.negate();
    assert(Value.isNegative());
  }

  return Value;
}

Expected<APIntWithSign> APIntWithSign::fromString(StringRef StrView) {
  StringRef OriginalStr = StrView;
  APIntWithSign ValueWithSign;
  APInt &Value = ValueWithSign.Value;
  bool &HasNegativeSign = ValueWithSign.IsSigned;

  if (StrView.empty())
    return reportError("Empty string is not a valid number");

  // We remove minus because StringRef::getAsInteger method overload for APInt
  // doesn't handle the minus.
  HasNegativeSign = StrView.consume_front("-");

  auto Radix = getAutoSenseRadix(StrView);

  Expected<APInt> ExpectedValue =
      parseAPInt(StrView, HasNegativeSign, Radix, OriginalStr);
  if (auto E = ExpectedValue.takeError())
    return reportError(toString(std::move(E)));

  Value = *ExpectedValue;
  return ValueWithSign;
}

void ValuegramBitValueEntry::mapYamlImpl(yaml::IO &Io) {
  Io.mapRequired("value", ValWithSign);
}

void ValuegramBitRangeEntry::mapYamlImpl(yaml::IO &Io) {
  auto MapUnsignedAPInt = [&](const char *Key, APInt &Val) {
    struct APIntBitsMapperHelper {
      APIntBitsMapperHelper(yaml::IO &Io) {}
      APIntBitsMapperHelper(yaml::IO &Io, APInt Val)
          : ValWithSign{std::move(Val), /*IsSigned=*/false} {}
      APInt denormalize(yaml::IO &Io) {
        if (ValWithSign.IsSigned)
          Io.setError("Value can't be negative");
        return ValWithSign.Value;
      }
      APIntWithSign ValWithSign;
    };

    yaml::MappingNormalization<APIntBitsMapperHelper, APInt> NormAPInt(Io, Val);
    Io.mapRequired(Key, NormAPInt->ValWithSign);
  };

  MapUnsignedAPInt("min", Min);
  MapUnsignedAPInt("max", Max);
}

static std::optional<EntryKind>
getValuegramSequenceEntryKind(StringRef ParseStr) {
  if (ParseStr == "uniform")
    return ValuegramPattern::Uniform;
  if (ParseStr == "bitpattern")
    return ValuegramPattern::BitPattern;
  return std::nullopt;
}

StringRef toString(EntryKind Kind) {
  switch (Kind) {
  case ValuegramPattern::Uniform:
    return "uniform";
  case ValuegramPattern::BitPattern:
    return "bitpattern";
  case EntryKind::BitValue:
    return "bitvalue";
  case EntryKind::BitRange:
    return "bitrange";
  case EntryKind::MapEntry:
  case EntryKind::HelperSentinel:
    return "<internal>";
  }
  return StringRef{"<invalid>"};
}

Expected<std::unique_ptr<IValuegramEntry>>
createValuegramSequenceEntry(IValuegramEntry::EntryKind Kind) {
  switch (Kind) {
  case ValuegramPattern::BitPattern:
    return std::make_unique<ValuegramBitpatternEntry>();
  case ValuegramPattern::Uniform:
    return std::make_unique<ValuegramUniformEntry>();
  case EntryKind::BitValue:
    return std::make_unique<ValuegramBitValueEntry>();
  default:
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             Twine("Can't create IValuegramSeqEntry of kind: ")
                                 .concat(toString(Kind)));
  }
}

std::unique_ptr<IValuegramEntry> createValuegramSeqEntry(yaml::IO &Io,
                                                         StringRef ParseStr) {
  auto Pattern = getValuegramSequenceEntryKind(ParseStr);

  if (Pattern)
    return cantFail(createValuegramSequenceEntry(*Pattern));

  auto ReportError = [&](const Twine &Msg) -> std::unique_ptr<IValuegramEntry> {
    Io.setError(Msg);
    return {};
  };

  Expected<APIntWithSign> ExpectedValue = APIntWithSign::fromString(ParseStr);
  if (auto E = ExpectedValue.takeError())
    return ReportError(toString(std::move(E)));

  return std::make_unique<ValuegramBitValueEntry>(*ExpectedValue);
}

std::unique_ptr<IValuegramEntry>
YAMLHistogramTraits<std::unique_ptr<IValuegramEntry>>::denormalizeEntry(
    yaml::IO &Io, StringRef ValueStr, double Weight) {
  auto Entry = createValuegramSeqEntry(Io, ValueStr);
  // In case createValuegramSeqEntry we have to return an empty husk
  // and provide a good error message via Io.setError(....).
  if (Entry)
    Entry->Weight = Weight;
  return Entry;
}

void YAMLHistogramTraits<std::unique_ptr<IValuegramEntry>>::normalizeEntry(
    yaml::IO &Io, const std::unique_ptr<IValuegramEntry> &E,
    SmallVectorImpl<SValue> &RawStrings) {
  using EntryKind = IValuegramEntry::EntryKind;
  EntryKind Kind = E->getKind();

  auto FirstVal = [&]() -> std::string {
    switch (Kind) {
    case EntryKind::BitValue: {
      auto &BitValueEntry = cast<ValuegramBitValueEntry>(E);
      SmallString<16> SmallStr;
      BitValueEntry.getVal().toString(SmallStr, /*Radix=*/16,
                                      BitValueEntry.isSigned(),
                                      /*formatAsCLiteral=*/true,
                                      /*UpperCase=*/false);
      return SmallStr.str().str();
    }
    case ValuegramPattern::BitPattern:
    case ValuegramPattern::Uniform:
      return toString(Kind).str();
    default:
      llvm_unreachable("IValuegramSeqEntry is of unexpected type");
    }
  }();

  RawStrings.push_back(FirstVal);
  RawStrings.push_back(std::to_string(E->Weight));
}

void IValuegramMapEntry::mapYaml(yaml::IO &Io) {
  Io.mapOptional("weight", Weight, 1.0);
  mapYamlImpl(Io);
}

Expected<ValuegramEntry> ValuegramEntry::create(EntryKind Kind) {
  switch (Kind) {
  case EntryKind::BitPattern:
    return ValuegramEntry{std::make_unique<ValuegramBitpatternEntry>()};
  case EntryKind::Uniform:
    return ValuegramEntry{std::make_unique<ValuegramUniformEntry>()};
  case EntryKind::BitValue:
    return ValuegramEntry{std::make_unique<ValuegramBitValueEntry>()};
  case EntryKind::BitRange:
    return ValuegramEntry{std::make_unique<ValuegramBitRangeEntry>()};
  default:
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        Twine("Can't create ValuegramEntry of kind: ").concat(toString(Kind)));
  }
}

Expected<ValuegramEntry>
ValuegramEntry::create(std::unique_ptr<IValuegramEntry> Owning) {
  if (!Owning)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Can't create ValuegramEntry from an empty "
                             "std::unique_ptr<IValuegramEntry>");

  auto Kind = Owning->getKind();
  switch (Kind) {
  case ValuegramPattern::BitPattern:
  case ValuegramPattern::Uniform:
  case EntryKind::BitValue:
  case EntryKind::BitRange:
    return ValuegramEntry{std::move(Owning)};
  default:
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        Twine("Can't create ValuegramEntry of kind: ").concat(toString(Kind)));
  }
}

} // namespace snippy

} // namespace llvm
