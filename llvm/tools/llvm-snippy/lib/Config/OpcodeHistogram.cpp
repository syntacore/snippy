//===-- OpcodeHistogram.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/ConfigIOContext.h"

// FIXME: remove this dependency (an interface should be introduced)
#include "snippy/Config/PluginWrapper.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include "snippy/Target/Target.h"

#include <optional>
#include <type_traits>

namespace llvm {

// all mapping shall live in llvm, not in llvm::snippy
using namespace snippy;

size_t yaml::SequenceTraits<OpcodeHistogramSequence>::size(
    IO &IO, OpcodeHistogramSequence &Seq) {
  return Seq.Data.size();
}

SequenceType &yaml::SequenceTraits<OpcodeHistogramSequence>::element(
    IO &IO, OpcodeHistogramSequence &Seq, size_t Index) {
  if (Index >= Seq.Data.size())
    Seq.Data.resize(Index + 1);
  return Seq.Data[Index];
}

void OpcodeHistogramSequence::validate(yaml::IO &IO) const {
  if (llvm::any_of(Data, [](auto &&StringSeq) { return StringSeq.size() < 2; }))
    IO.setError("Incorrect histogram: Key and weight must be specified and "
                "separated with a comma");
}

OpcodeHistogramNormalization::OpcodeHistogramNormalization(yaml::IO &IO) {
  mapHistogramPatterns(IO);
}

OpcodeHistogramNormalization::OpcodeHistogramNormalization(
    yaml::IO &IO, const OpcodeHistogram &HistData) {
  mapHistogramPatterns(IO);
  const auto &CfgIOContext =
      static_cast<const ConfigIOContext *>(IO.getContext());
  assert(CfgIOContext);
  const auto &OpCC = CfgIOContext->OpCC;
  auto InsertHistogramEntry = [&](const std::string &Name, double Weight,
                                  yaml::NodeKind EntityKind) {
    OpcodeHistogramSequence::SequenceType EntrySeq;
    EntrySeq.emplace_back(Name, EntityKind);
    EntrySeq.emplace_back(std::to_string(Weight), yaml::NodeKind::Scalar);
    OpcHistSeq.Data.push_back(std::move(EntrySeq));
  };
  // Normalize top opcodes and patterns
  for (auto &&[Opc, Weight] : HistData.topOpcodes())
    InsertHistogramEntry(OpCC.name(Opc).str(), Weight, yaml::NodeKind::Scalar);
  for (auto &&[Pattern, Weight] : HistData.patterns()) {
    auto *HistNode = dyn_cast<HistogramNode>(Pattern);
    assert(HistNode);
    InsertHistogramEntry(HistNode->getName(), Weight, yaml::NodeKind::Map);
  }
}

OpcodeHistogram OpcodeHistogramNormalization::denormalize(yaml::IO &IO) {
  OpcHistSeq.validate(IO);
  if (IO.error())
    return {};

  auto NameWeightSeq = OpcHistSeq.getEntryWeightSequence();
  OpcodeHistogram Result;
  if (!DefineMainHist.empty() && DefineMainHist != "histogram") {
    if (auto ErrStr = insertHistogramNode(Result, DefineMainHist,
                                          BaseNode::DefaultNodeWeight);
        !ErrStr.empty()) {
      IO.setError(ErrStr);
      return {};
    }
    Result.reinitProbabilityVisitor();
    return Result;
  }
  for (auto &&[NameInfo, WeightStr] : NameWeightSeq) {
    auto Weight = parseWeight(WeightStr);
    if (!Weight) {
      IO.setError(NameInfo.Val + " is given with incorrect weight: " +
                  llvm::toString(Weight.takeError()));
      break;
    }

    auto Name = NameInfo.Val;
    if (NameInfo.Kind == yaml::NodeKind::Map) {
      if (auto ErrStr = insertHistogramNode(Result, Name, Weight.get());
          !ErrStr.empty()) {
        IO.setError(ErrStr);
        return {};
      }
    } else if (NameInfo.Kind == yaml::NodeKind::Scalar) {
      auto DecodeEntry = decodeInstrRegex(IO, NameInfo.Val, Weight.get());
      if (!DecodeEntry) {
        IO.setError(llvm::toString(DecodeEntry.takeError()));
        return {};
      }
      for (auto &&[Opc, WeightRes] : DecodeEntry->Decoded)
        Result.insertTopOpcode(Opc, WeightRes);
    }
  }
  // Recalculate opcode probabilities for the fully constructed histogram
  Result.reinitProbabilityVisitor();
  return Result;
}

void OpcodeHistogramNormalization::mapHistogramPatterns(yaml::IO &IO) {
  auto *CurrContext = IO.getContext();
  assert(CurrContext);
  auto *ConfigIOCtx = static_cast<ConfigIOContext *>(CurrContext);
  assert(ConfigIOCtx);
  HistogramPatternsContext HistCtx(*ConfigIOCtx, HistPatterns);
  // We swap the context during mapping to access specific objects
  IO.setContext(&HistCtx);
  IO.mapOptional("histogram-patterns", HistPatterns);
  // Return the previous context
  IO.setContext(CurrContext);
}

std::string OpcodeHistogramNormalization::insertHistogramNode(
    OpcodeHistogram &Hist, StringRef Name, double Weight) {
  if (!HistPatterns.contains(Name))
    return llvm::formatv("unknown pattern: '{0}'", Name);
  if (isa<HistogramNode>(HistPatterns.get(Name)))
    Hist.insert(HistPatterns.clone(Name));
  else
    Hist.emplace<HistogramNode>(Name, HistPatterns.clone(Name), Weight);
  Hist.HasPatterns = true;
  return "";
}

void yaml::MappingTraits<snippy::OpcodeHistogramMappingWrapper>::mapping(
    yaml::IO &Io, snippy::OpcodeHistogramMappingWrapper &Info) {
  yaml::MappingNormalization<OpcodeHistogramNormalization, OpcodeHistogram>
      HistNorm(Io, Info.Histogram);
  Io.mapOptional("histogram", HistNorm->OpcHistSeq);
}

Expected<OpcodeHistogramDecodedEntry>
decodeInstrRegex(yaml::IO &IO, StringRef OpcodeStr, double Weight) {
  const auto &CfgIOContext =
      static_cast<const ConfigIOContext *>(IO.getContext());
  assert(CfgIOContext);
  const auto &OpCC = CfgIOContext->OpCC;

  auto ReportError = [&](Twine Msg) -> Error {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             Msg);
  };

  auto ReportNoMatchesError = [&](Twine OpcodeStr) -> Error {
    return ReportError("Illegal opcode for specified cpu: " + Twine(OpcodeStr) +
                       "\nUse -list-opcode-names option "
                       "to check for available instructions!");
  };

  if (Regex::isLiteralERE(OpcodeStr)) {
    auto Opcode = OpCC.code(OpcodeStr.str());
    if (!Opcode)
      return ReportNoMatchesError(OpcodeStr);

    return OpcodeHistogramDecodedEntry{{*Opcode, Weight}};
  }

  OpcodeHistogramDecodedEntry Result(OpcodeStr);
  auto OpcodeRegexp = createWholeWordMatchRegex(OpcodeStr);
  if (auto Err = OpcodeRegexp.takeError())
    return ReportError("Illegal opcode regular expression \"" + OpcodeStr +
                       "\": " + Twine(toString(std::move(Err))));

  SmallVector<unsigned, 16> MatchedOpcodes;
  OpCC.code(*OpcodeRegexp, MatchedOpcodes);
  transform(
      MatchedOpcodes, std::back_inserter(Result.Decoded),
      [IndividualWeight = Weight / MatchedOpcodes.size()](unsigned Opcode) {
        return OpcodeHistogramEntry{Opcode, IndividualWeight};
      });

  if (MatchedOpcodes.empty())
    return ReportNoMatchesError(OpcodeStr);
  return Result;
}

OpcodeHistogramCodedEntry
codeInstrFromOpcode(yaml::IO &IO, const OpcodeHistogramDecodedEntry &E) {
  auto &Decoded = E.Decoded;
  assert(Decoded.size() == 1 && "Expected entry to contain only a "
                                "single opcode, can't serialize");
  auto &First = Decoded.front();
  const auto &CfgIOContext =
      static_cast<const ConfigIOContext *>(IO.getContext());
  assert(CfgIOContext);
  const auto &OpCC = CfgIOContext->OpCC;
  return OpcodeHistogramCodedEntry{std::string(OpCC.name(First.Opcode)),
                                   std::to_string(First.Weight)};
}

unsigned OpcodeHistogram::getCFInstrsNum(unsigned InstrsNum,
                                         const OpcodeCache &OpCC) const {
  // CF instructions can only be present in the top-level histogram.
  double CFInstrsWeight =
      std::accumulate(TopOpcodes.begin(), TopOpcodes.end(), 0.0,
                      [&OpCC](double Accumulation, auto &&Hist) -> double {
                        auto *Desc = OpCC.desc(Hist.first);
                        if (Desc && Desc->isBranch())
                          return Accumulation + Hist.second;
                        return Accumulation;
                      });
  double TotalWeight = getTotalChildsWeight();

  double CFInstrsRatio = CFInstrsWeight / TotalWeight;
  if (!std::isfinite(CFInstrsRatio))
    return 0;

  double CFInstrsNum = InstrsNum * CFInstrsRatio;
  if (CFInstrsNum > std::numeric_limits<int>::max())
    return std::numeric_limits<int>::max();

  if (!std::isnan(CFInstrsNum) && (CFInstrsNum >= 1.0))
    return static_cast<int>(CFInstrsNum);

  return 0;
}

bool OpcodeHistogram::contains(unsigned OpcodeToFind) const {
  return ProbVisitor.opcodeProbabilities().count(OpcodeToFind);
}

OpcodeProbVisitor::OpcProbOpt
OpcodeHistogram::find(unsigned OpcodeToFind) const {
  return ProbVisitor.find(OpcodeToFind);
}

const OpcodeProbVisitor::OpcodeProbsType &
OpcodeHistogram::opcodeProbabilities() const {
  return ProbVisitor.opcodeProbabilities();
}

double
OpcodeHistogram::getTopOpcodesWeight(std::function<bool(unsigned)> Pred) const {
  auto TopOpcodesFiltered = llvm::make_filter_range(
      TopOpcodes, [&](auto &&OpcWeight) { return Pred(OpcWeight.first); });
  auto Weights = llvm::make_second_range(TopOpcodesFiltered);
  return std::accumulate(Weights.begin(), Weights.end(), /* init */ 0.0);
}

bool OpcodeHistogram::hasCallInstrs(const OpcodeCache &OpCC,
                                    const SnippyTarget &Tgt) const {
  // Call instructions can only be present in the top-level histogram.
  return llvm::any_of(TopOpcodes, [&OpCC, &Tgt](auto &Hist) {
    auto *Desc = OpCC.desc(Hist.first);
    return Desc && Tgt.isCall(Desc->getOpcode());
  });
}

bool OpcodeHistogram::hasSPRelativeInstrs(const OpcodeCache &OpCC,
                                          const SnippyTarget &Tgt) const {
  auto Pred = [&OpCC, &Tgt](unsigned Opc) {
    auto *Desc = OpCC.desc(Opc);
    return Desc && Tgt.isSPRelative(Desc->getOpcode());
  };
  return ProbVisitor.find(Pred).has_value();
}

bool OpcodeHistogram::hasPlainInstrs(const OpcodeCache &OpCC,
                                     const SnippyTarget &Tgt) const {
  auto Opcodes = uniqueOpcodes();
  return llvm::any_of(Opcodes, [&OpCC, &Tgt](unsigned Opcode) {
    auto *Desc = OpCC.desc(Opcode);
    return Desc && !Tgt.isCall(Desc->getOpcode()) && !Desc->isBranch();
  });
}

} // namespace llvm
