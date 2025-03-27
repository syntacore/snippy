//===-- OpcodeHistogram.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLHistogram.h"
// FIXME: remove this dependency (an interface should be introduced)
#include "snippy/Config/PluginWrapper.h"

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include "snippy/Target/Target.h"

#include <optional>
#include <sstream>
#include <type_traits>

namespace llvm {

// all mapping shall live in llvm, not in llvm::snippy
using namespace snippy;

Expected<OpcodeHistogramDecodedEntry>
decodeInstrRegex(yaml::IO &IO, StringRef OpcodeStr, double Weight) {
  auto &ParserCtx = snippy::YAMLHistogramTraits<
      snippy::OpcodeHistogramDecodedEntry>::getContext(IO);
  const auto &OpCC = ParserCtx.OpCC;

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
  SmallString<32> NameStorage;
  (Twine("^(") + OpcodeStr + ")$").toVector(NameStorage);
  Regex OpcodeRegexp(NameStorage);
  std::string Error;
  if (!OpcodeRegexp.isValid(Error))
    return ReportError("Illegal opcode regular expression \"" + OpcodeStr +
                       "\": " + Twine(Error));

  SmallVector<unsigned, 16> MatchedOpcodes;
  OpCC.code(OpcodeRegexp, MatchedOpcodes);
  transform(
      MatchedOpcodes, std::back_inserter(Result.Decoded),
      [IndividualWeight = Weight / MatchedOpcodes.size()](unsigned Opcode) {
        return OpcodeHistogramEntry{Opcode, IndividualWeight};
      });

  if (MatchedOpcodes.empty())
    return ReportNoMatchesError(OpcodeStr);
  return Result;
}

OpcodeHistogramDecodedEntry
YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::denormalizeEntry(
    yaml::IO &IO, StringRef OpcodeStr, double Weight) {
  Expected<OpcodeHistogramDecodedEntry> Decoded =
      decodeInstrRegex(IO, OpcodeStr, Weight);
  if (!Decoded) {
    IO.setError(toString(Decoded.takeError()));
    return OpcodeHistogramDecodedEntry{};
  }

  return *Decoded;
}

OpcodeHistogramCodedEntry
codeInstrFromOpcode(yaml::IO &IO, const OpcodeHistogramDecodedEntry &E) {
  auto &Decoded = E.Decoded;
  assert(Decoded.size() == 1 && "Expected entry to contain only a "
                                "single opcode, can't serialize");
  auto &First = Decoded.front();
  const auto &OpCC = snippy::YAMLHistogramTraits<
                         snippy::OpcodeHistogramDecodedEntry>::getContext(IO)
                         .OpCC;
  return OpcodeHistogramCodedEntry{std::string(OpCC.name(First.Opcode)),
                                   std::to_string(First.Weight)};
}

void YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::normalizeEntry(
    yaml::IO &IO, const DenormEntry &E, SmallVectorImpl<SValue> &RawStrings) {
  auto Result = codeInstrFromOpcode(IO, E);
  RawStrings.push_back(Result.InstrMnemonic);
  RawStrings.push_back(Result.Weight);
}

OpcodeHistogram
YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::denormalizeMap(
    yaml::IO &IO, ArrayRef<DenormEntry> Entries) {
  auto &ParserCtx = getContext(IO);
  auto &Ctx = ParserCtx.Ctx;
  const auto &OpCC = ParserCtx.OpCC;

  MapType Histogram;
  for (const auto &HEntry : Entries) {
    if (HEntry.Decoded.empty())
      continue;

    if (HEntry.RegexPattern.empty()) {
      assert(HEntry.Decoded.size() == 1 && "Expected to have a single opcode");

      auto &&[Opcode, Weight] = HEntry.Decoded.front();
      bool Inserted = Histogram.insert_or_assign(Opcode, Weight).second;
      if (!Inserted)
        snippy::warn(WarningName::InstructionHistogram, Ctx,
                     "Repeated instruction in histogram",
                     "Replacing weight for instruction " + OpCC.name(Opcode));
      continue;
    }

    for (auto &&[Opcode, Weight] : HEntry.Decoded) {
      // Don't overwrite weight if it was previously set
      bool Inserted = Histogram.insert({Opcode, Weight}).second;
      if (!Inserted)
        snippy::warn(WarningName::InstructionHistogram, Ctx,
                     "Repeated instruction in histogram",
                     "Keeping old weight for instruction " + OpCC.name(Opcode) +
                         " matched by pattern \"" + HEntry.RegexPattern + "\"");
    }
  }

  return Histogram;
}

void YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::normalizeMap(
    yaml::IO &IO, const MapType &Hist, std::vector<DenormEntry> &Entries) {
  transform(Hist, std::back_inserter(Entries), [](auto Pair) {
    auto &&[Opcode, Weight] = Pair;
    return DenormEntry{{Opcode, Weight}};
  });
}

namespace snippy {
void diagnoseHistogram(LLVMContext &Ctx, const OpcodeCache &OpCC,
                       std::map<unsigned, double> &Histogram) {
  if (Histogram.size() == 0) {
    snippy::warn(WarningName::InstructionHistogram, Ctx,
                 "Plugin didn't fill histogram",
                 "Generating instructions with only plugin calls");
    return;
  }

  auto InvalidOpcChecker = [OpCC](auto It) {
    return OpCC.desc(It.first) == nullptr;
  };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidOpcChecker) !=
      Histogram.end())
    snippy::fatal("Plugin filled histogram with invalid opcodes");

  auto InvalidWeightsChecker = [](auto It) { return It.second < 0; };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidWeightsChecker) !=
      Histogram.end())
    snippy::fatal("Plugin filled histogram with negative opcodes weights");
}

bool OpcodeHistogram::hasCallInstrs(const OpcodeCache &OpCC,
                                    const SnippyTarget &Tgt) const {
  return std::any_of(begin(), end(), [&OpCC, &Tgt](auto &Hist) {
    auto *Desc = OpCC.desc(Hist.first);
    return Desc && Tgt.isCall(Desc->getOpcode());
  });
}

} // namespace snippy
} // namespace llvm
