//===-- OperandsReinitialization.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/OperandsReinitialization.h"
#include "snippy/Config/Valuegram.h"
#include "snippy/Support/APIntSampler.h"
#include "snippy/Support/YAMLExtras.h"
#include "snippy/Support/YAMLHistogram.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"

#include <unordered_set>

#define DEBUG_TYPE "snippy-config-operands-reinitialization"

namespace llvm {

using EntryKind = snippy::IOpcodeValuegramEntry::EntryKind;

void yaml::ScalarEnumerationTraits<EntryKind>::enumeration(yaml::IO &IO,
                                                           EntryKind &Kind) {
  IO.enumCase(Kind, "uniform", EntryKind::Uniform);
  IO.enumCase(Kind, "operands", EntryKind::Operands);
}

using snippy::OperandsReinitDataSource;
void yaml::MappingTraits<OperandsReinitDataSource>::mapping(
    yaml::IO &IO, OperandsReinitDataSource &Cfg) {
  double &Weight = Cfg.Weight;
  OperandsReinitDataSource::Kind &Type = Cfg.DSKind;
  auto &Valuegram = Cfg.Valuegram;

  IO.mapRequired("type", Type);
  IO.mapOptional("weight", Weight);
  if (Type == OperandsReinitDataSource::Kind::Valuegram) {
    IO.mapOptional("opcodes", Valuegram);
  }
}

std::string yaml::MappingTraits<OperandsReinitDataSource>::validate(
    yaml::IO &IO, OperandsReinitDataSource &Cfg) {
  if (Cfg.getWeight() < 0)
    return "Operands reinitialization data source weight can not be less than "
           "0";

  assert(Cfg.getKind() == OperandsReinitDataSource::Kind::Valuegram &&
         "Only the valuegram data source type is supported");
  if (!Cfg.Valuegram.has_value())
    return "Valuegram data source must contain 'opcodes' key. Please, provide "
           "the opcode valuegram";

  for (auto &&RegEx : Cfg.getValuegram()) {
    const auto &RegExData = RegEx.Data;
    auto Entries = map_range(RegExData, [](auto &&Entry) {
      return dyn_cast_or_null<snippy::IOpcodeValuegramMapEntry>(
          Entry.getOrNull());
    });

    for (auto *Entry : Entries) {
      if (!Entry)
        continue; // N.B. invalid entries will be null
      if (auto Msg = Entry->validate(IO); Msg.size())
        return Msg;
    }
  }
  return "";
}

void yaml::MappingTraits<const snippy::OpcodeValuegramEntryMapMapper>::mapping(
    yaml::IO &IO, const snippy::OpcodeValuegramEntryMapMapper &Mapper) {
  using EntryKind = snippy::IOpcodeValuegramEntry::EntryKind;

  auto &Entry = Mapper.Entry;
  EntryKind Kind =
      IO.outputting() ? Entry.getKind() : EntryKind::HelperSentinel;
  IO.mapRequired("type", Kind);

  if (!IO.outputting())
    if (Error E =
            snippy::OpcodeValuegramOpcSettingsEntry::create(Kind).moveInto(
                Entry)) {
      consumeError(std::move(E));
      return;
    }

  auto &MapEntry = cast<snippy::IOpcodeValuegramMapEntry>(Entry.get());
  MapEntry.mapYaml(IO);
}

void yaml::ScalarEnumerationTraits<OperandsReinitDataSource::Kind>::enumeration(
    yaml::IO &IO, OperandsReinitDataSource::Kind &Kind) {
  IO.enumCase(Kind, "valuegram", OperandsReinitDataSource::Kind::Valuegram);
}

LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(
    std::unique_ptr<snippy::IOpcodeValuegramEntry>)

namespace snippy {

static std::optional<EntryKind>
getOpcodeValuegramSequenceEntryKind(StringRef ParseStr) {
  if (ParseStr == "uniform")
    return EntryKind::Uniform;
  return std::nullopt;
}

static std::unique_ptr<IOpcodeValuegramEntry>
createOpcodeValuegramSeqEntry(yaml::IO &IO, StringRef Str) {
  auto ReportError =
      [&](const Twine &Msg) -> std::unique_ptr<IOpcodeValuegramEntry> {
    IO.setError(Msg);
    return {};
  };

  auto Kind = getOpcodeValuegramSequenceEntryKind(Str);
  if (Kind)
    switch (Kind.value()) {
    case EntryKind::Uniform:
      return std::make_unique<OpcodeValuegramUniformEntry>();
    default:
      return ReportError(Twine("Can't create IOpcodeValuegramEntry of kind: ")
                             .concat(toString(Kind.value())));
    }
  return ReportError("Unsupported opcode valuegram sequency entry");
}

std::unique_ptr<IOpcodeValuegramEntry>
YAMLHistogramTraits<std::unique_ptr<IOpcodeValuegramEntry>>::denormalizeEntry(
    yaml::IO &Io, StringRef ValueStr, double Weight) {
  auto Entry = createOpcodeValuegramSeqEntry(Io, ValueStr);
  // In case createValuegramSeqEntry we have to return an empty husk
  // and provide a good error message via Io.setError(....).
  if (Entry)
    Entry->getWeight() = Weight;
  return Entry;
}

void YAMLHistogramTraits<std::unique_ptr<IOpcodeValuegramEntry>>::
    normalizeEntry(yaml::IO &Io,
                   const std::unique_ptr<IOpcodeValuegramEntry> &Entry,
                   SmallVectorImpl<SValue> &RawStrings) {
  using EntryKind = IOpcodeValuegramEntry::EntryKind;
  EntryKind Kind = Entry->getKind();

  RawStrings.push_back(std::string(toString(Kind)));
  RawStrings.push_back(std::to_string(Entry->getWeight()));
}

void IOpcodeValuegramMapEntry::mapYaml(yaml::IO &IO) {
  IO.mapOptional("weight", Weight, 1.0);
  mapYamlImpl(IO);
}

void OpcodeValuegramOperandsEntry::mapYamlImpl(yaml::IO &IO) {
  IO.mapRequired("values", Values);
}

StringRef toString(IOpcodeValuegramEntry::EntryKind Kind) {
  switch (Kind) {
  case IOpcodeValuegramEntry::EntryKind::Uniform:
    return "uniform";
  case IOpcodeValuegramEntry::EntryKind::Operands:
    return "operands";
  case IOpcodeValuegramEntry::EntryKind::MapEntry:
  case IOpcodeValuegramEntry::EntryKind::HelperSentinel:
    return "<internal>";
  };
  return "invalid";
}

template <>
std::unique_ptr<IOperandAPIntSampler>
WeightedAPIntSamplerSetBuilder<IOperandAPIntSampler>::build() {
  return std::make_unique<WeightedOperandAPIntSamplerSet>(WeightedSamplers);
}

static std::unique_ptr<IOperandAPIntSampler>
createSamplerForVal(const OperandsValuesEntry &Val) {
  return std::visit(
      OverloadedCallable(
          [](const std::monostate &) -> std::unique_ptr<IOperandAPIntSampler> {
            return std::make_unique<OpcodeValuegramUniformSampler>();
          },
          [](const FormattedAPIntWithSign &Val)
              -> std::unique_ptr<IOperandAPIntSampler> {
            return std::make_unique<OpcodeValuegramConstantSampler>(
                Val.getVal());
          },
          [](const Valuegram &Vgram) -> std::unique_ptr<IOperandAPIntSampler> {
            return std::make_unique<OpcodeValuegramValuegramSampler>(Vgram);
          }),
      Val.get());
}

OpcodeValuegramOperandsSampler::OpcodeValuegramOperandsSampler(
    const OpcodeValuegramOperandsEntry &Entry) {
  transform(Entry.Values, std::back_inserter(ValSamplers),
            [&](const auto &Val) { return createSamplerForVal(Val); });
}

static std::unique_ptr<IOperandAPIntSampler>
createSamplerForOpcSettings(const OpcodeValuegramOpcSettingsEntry &Entry) {
  return TypeSwitch<const IOpcodeValuegramEntry *,
                    std::unique_ptr<IOperandAPIntSampler>>(&Entry.get())
      .Case([](const OpcodeValuegramUniformEntry *) {
        return std::make_unique<OpcodeValuegramUniformSampler>();
      })
      .Case([](const OpcodeValuegramOperandsEntry *Operands) {
        return std::make_unique<OpcodeValuegramOperandsSampler>(*Operands);
      })
      .Default([](auto &&) -> std::unique_ptr<IOperandAPIntSampler> {
        llvm_unreachable("Unhandled OpcodeValuegramOpcSettingsEntry type");
      });
}

OpcodeSettingsSampler::OpcodeSettingsSampler(
    const OpcodeValuegramSettings &Settings)
    : Cfg(Settings) {
  assert(!Settings.empty() && "Can't construct empty weighted sampler set");
  WeightedAPIntSamplerSetBuilder<IOperandAPIntSampler> Builder;
  for (auto &&Entry : Settings)
    Builder.addOwned(createSamplerForOpcSettings(Entry), Entry.getWeight());
  ContainedSampler = Builder.build();
}

static void checkMatchedRegExes(
    const OpcodesValuegram &OpcVal,
    const std::unordered_set<const OpcodeValuegramRegEx *> &Matched) {
  auto NotMatched = make_filter_range(
      OpcVal, [&Matched](const auto &Conf) { return !Matched.count(&Conf); });
  llvm::for_each(NotMatched, [](const auto &Conf) {
    snippy::warn(WarningName::InconsistentOptions,
                 "Unused regex in opcode valuegram",
                 "No opcode has been matched by \"" + Twine(Conf.Expr) + "\".");
  });
}

OpcodeToSettingsMap::OpcodeToSettingsMap(const OpcodesValuegram &OpcVal,
                                         const OpcodeHistogram &OpcHist,
                                         const OpcodeCache &OpCC) {
  unsigned NMatched = 0;
  SmallVector<StringRef> Matches;
  // To avoid false unused regex warning
  std::unordered_set<const OpcodeValuegramRegEx *> Matched;
  for (auto Opc : make_first_range(OpcHist)) {
    for (auto &&Conf : OpcVal) {
      assert(NMatched <= OpcHist.size());
      if (NMatched == OpcHist.size()) {
        break;
      }
      auto RX = createWholeWordMatchRegex(Conf.Expr);
      if (!RX)
        // NOTE: The regex has been validated during parsing, this shouldn't
        // happen.
        snippy::fatal(RX.takeError());
      StringRef Name = OpCC.name(Opc);
      auto Res = RX->match(Name, &Matches);
      if (Res) {
        auto Inserted = Data.emplace(Opc, Conf.Data);
        if (Inserted.second) {
          Matched.emplace(&Conf);
          ++NMatched;
          LLVM_DEBUG(dbgs() << "Opcode Histogram matched opcode \"" << Name
                            << "\" with regex \"" << Conf.Expr << "\".\n");
        }
      }
    }
    // No regex matches the opcode in this valuegram
    auto Inserted = Data.emplace(Opc, OpcodeValuegramSettings{});
    if (Inserted.second) {
      LLVMContext Ctx;
      snippy::notice(WarningName::NotAWarning, Ctx,
                     "No regex that matches \"" + Twine(OpCC.name(Opc)) +
                         "\" was found in opcode valuegram",
                     "This instruction will not be reinitializated.");
    }
  }
  checkMatchedRegExes(OpcVal, Matched);
}

WeightedOpcToSettingsMaps::WeightedOpcToSettingsMaps(
    const OperandsReinitializationConfig &ORConfig,
    const OpcodeHistogram &OpcHist, const OpcodeCache &OpCC) {
  for (auto &&DataSource : ORConfig) {
    assert(DataSource.isValuegram() &&
           "Unrecognized operands reinitialization data source type");
    double Weight = DataSource.getWeight();
    Maps.emplace_back(
        OpcodeToSettingsMap{DataSource.getValuegram(), OpcHist, OpCC}, Weight);
  }
}

static Expected<std::unique_ptr<IOperandAPIntSampler>>
createSamplerForValuegramEntry(const ValuegramEntry &Entry) {
  return TypeSwitch<const IValuegramEntry *,
                    Expected<std::unique_ptr<IOperandAPIntSampler>>>(
             &Entry.get())
      .Case([&](const ValuegramBitpatternEntry *) {
        return std::make_unique<OpcodeValuegramBitpatternSampler>();
      })
      .Case([&](const ValuegramUniformEntry *) {
        return std::make_unique<OpcodeValuegramUniformSampler>();
      })
      .Case([&](const ValuegramBitValueEntry *BitValueEntry) {
        assert(BitValueEntry);
        const auto &Val = BitValueEntry->ValWithSign.getVal();
        return std::make_unique<OpcodeValuegramConstantSampler>(Val);
      })
      .Case([&](const ValuegramBitRangeEntry *BitRangeEntry) {
        assert(BitRangeEntry);
        auto &Min = BitRangeEntry->Min, &Max = BitRangeEntry->Max;
        return std::make_unique<OpcodeValuegramRangeSampler>(
            Min, Max, /*IsSigned*/ false);
      })
      .Default([](auto &&) -> Expected<std::unique_ptr<IOperandAPIntSampler>> {
        llvm_unreachable("Unhandled valuegram entry type");
      });
}

OpcodeValuegramValuegramSampler::OpcodeValuegramValuegramSampler(
    const Valuegram &Cfg)
    : Cfg(Cfg) {
  WeightedAPIntSamplerSetBuilder<IOperandAPIntSampler> Builder;
  for (auto &&Entry : Cfg) {
    auto SamplerOrErr = createSamplerForValuegramEntry(Entry);
    if (!SamplerOrErr)
      snippy::fatal("Internal error", SamplerOrErr.takeError());
    Builder.addOwned(std::move(*SamplerOrErr), Entry.getWeight());
  }
  OwnedSampler = Builder.build();
}

} // namespace snippy

void yaml::yamlize(IO &IO, const snippy::OpcodeValuegramEntryScalarMapper &Seq,
                   bool, EmptyContext &Ctx) {
  IO.setError("opcode valuegram entries cannot be specified as scalars");
}

void yaml::yamlize(IO &IO, const snippy::OperandsValuesEntryMapMapper &Seq,
                   bool, EmptyContext &Ctx) {
  IO.setError("'operands' values cannot be specified as maps");
}

using snippy::OpcodeValuegramSettings;
size_t yaml::SequenceTraits<OpcodeValuegramSettings, void>::size(
    yaml::IO &IO, OpcodeValuegramSettings &Valuegram) {
  return Valuegram.size();
}

snippy::OpcodeValuegramOpcSettingsEntry &
yaml::SequenceTraits<OpcodeValuegramSettings, void>::element(
    yaml::IO &IO, OpcodeValuegramSettings &Valuegram, size_t Index) {
  if (Valuegram.size() < Index + 1)
    Valuegram.resize(Index + 1, snippy::OpcodeValuegramOpcSettingsEntry{});
  return Valuegram[Index];
}

using snippy::OpcodeValuegramRegEx;
template <> struct yaml::CustomMappingTraits<OpcodeValuegramRegEx> {
  static void inputOne(IO &IO, StringRef Key, OpcodeValuegramRegEx &Cfg) {
    IO.mapRequired(Key.data(), Cfg.Data);
    Cfg.Expr = Key.str();
    auto RX = snippy::createWholeWordMatchRegex(Cfg.Expr);
    if (!RX)
      IO.setError(Twine("Invalid regex: ").concat(toString(RX.takeError())));
  }

  static void output(IO &IO, OpcodeValuegramRegEx &Cfg) {
    IO.mapRequired(Cfg.Expr.c_str(), Cfg.Data);
  }
};

} // namespace llvm
