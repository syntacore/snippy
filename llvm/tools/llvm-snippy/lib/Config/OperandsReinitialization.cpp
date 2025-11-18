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

OpcodeToValuegramMap::OpcodeToValuegramMap(const OpcodesValuegram &OpcVal,
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
    auto Inserted = Data.emplace(Opc, OpcodeValuegramOpcodeSettings{});
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

CommonOpcodeToValuegramMap::CommonOpcodeToValuegramMap(
    const OperandsReinitializationConfig &ORConfig,
    const OpcodeHistogram &OpcHist, const OpcodeCache &OpCC) {
  for (auto &&Sampler : ORConfig) {
    assert(Sampler.isValuegram() &&
           "Unrecognized operands reinitialization data source type");
    double Weight = Sampler.getWeight();
    auto Valuegram = Sampler.getValuegram();
    MapsAndWeights.emplace_back(OpcodeToValuegramMap{Valuegram, OpcHist, OpCC},
                                Weight);
  }
}

SmallVector<double>
CommonOpcodeToValuegramMap::createContainingMapsWeightsRange(
    unsigned Opcode) const {
  SmallVector<double> Result;
  llvm::transform(MapsAndWeights, std::back_inserter(Result),
                  [&Opcode](const auto &MapAndWeight) {
                    if (MapAndWeight.first.count(Opcode))
                      return MapAndWeight.second;
                    return 0.0;
                  });
  return Result;
}

const OpcodeValuegramOpcodeSettings &
CommonOpcodeToValuegramMap::getConfigForOpcode(unsigned Opc,
                                               const OpcodeCache &OpCC) const {
  SmallVector<double> Weights = createContainingMapsWeightsRange(Opc);
  Distribution MapsDist{Weights.begin(), Weights.end()};
  auto MapIdx = MapsDist(RandEngine::engine());
  assert(MapIdx < MapsAndWeights.size());
  auto &Map = MapsAndWeights[MapIdx].first;
  return Map.getConfigForOpcode(Opc, OpCC);
}

static APInt sampleOperandsValuesFromORConfig(const OperandsValuesEntry &Entry,
                                              unsigned NumBits) {
  switch (Entry.getKind()) {
  case OperandsValuesEntry::EntryKind::Uniform:
    return UniformAPIntSamler::generate(NumBits);
  case OperandsValuesEntry::EntryKind::Value:
    return Entry.getValue().getVal();
  }
  llvm_unreachable("Unrecognized OperandsValuesEntry kind");
}

std::optional<APInt> sampleOpcodeValuegramForOneReg(
    const OpcodeValuegramOpcodeSettings &OpcValuegram, unsigned OperandIndex,
    unsigned NumBits, std::discrete_distribution<size_t> &Dist, unsigned Opcode,
    [[maybe_unused]] const MCInstrInfo &InstrInfo) {
  // That means that opcode wasn't specified in the opcode valuegram so we
  // don't reinitialize its operands.
  if (OpcValuegram.empty())
    return std::nullopt;
  auto Index = Dist(RandEngine::engine());
  assert(Index < OpcValuegram.size());
  const auto &Entry = OpcValuegram[Index];
  auto Kind = Entry.getKind();

  using EntryKind = IOpcodeValuegramEntry::EntryKind;
  switch (Kind) {
  case EntryKind::Uniform:
    return UniformAPIntSamler::generate(NumBits);
  case EntryKind::Operands: {
    const auto &OperandsValues =
        cast<OpcodeValuegramOperandsEntry>(Entry.get()).Values;
    if (OperandsValues.size() <= OperandIndex)
      snippy::fatal("Operands reinitialization error",
                    "Incorrect number of operands values for \"" +
                        InstrInfo.getName(Opcode) + "\"");
    return sampleOperandsValuesFromORConfig(OperandsValues[OperandIndex],
                                            NumBits);
  }
  default:
    llvm_unreachable("unrecognized OpcodeValuegram entry type");
  }
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

void yaml::yamlize(IO &IO, const snippy::OperandsValuesEntrySeqMapper &Seq,
                   bool, EmptyContext &Ctx) {
  IO.setError("'operands' values cannot be specified as sequences");
}

using snippy::OpcodeValuegramOpcodeSettings;
size_t yaml::SequenceTraits<OpcodeValuegramOpcodeSettings, void>::size(
    yaml::IO &IO, OpcodeValuegramOpcodeSettings &Valuegram) {
  return Valuegram.size();
}

snippy::OpcodeValuegramOpcSettingsEntry &
yaml::SequenceTraits<OpcodeValuegramOpcodeSettings, void>::element(
    yaml::IO &IO, OpcodeValuegramOpcodeSettings &Valuegram, size_t Index) {
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
