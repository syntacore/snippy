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

OpcodeHistogramDecodedEntry
YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::denormalizeEntry(
    yaml::IO &IO, StringRef OpcodeStr, double Weight) {
  auto &ParserCtx = getContext(IO);
  const auto &OpCC = ParserCtx.OpCC;

  if (Regex::isLiteralERE(OpcodeStr)) {
    auto Opcode = OpCC.code(OpcodeStr.str());
    if (!Opcode)
      return reportNoMatchesError(IO, OpcodeStr);
    return OpcodeHistogramDecodedEntry{{*Opcode, Weight}};
  }

  OpcodeHistogramDecodedEntry Result(OpcodeStr);
  SmallString<32> NameStorage;
  (Twine("^(") + OpcodeStr + ")$").toVector(NameStorage);
  Regex OpcodeRegexp(NameStorage);
  std::string Error;
  if (!OpcodeRegexp.isValid(Error))
    return reportError(
        IO, "Illegal opcode regular expression \"" + OpcodeStr + "\"", Error);

  SmallVector<unsigned, 16> MatchedOpcodes;
  OpCC.code(OpcodeRegexp, MatchedOpcodes);
  transform(
      MatchedOpcodes, std::back_inserter(Result.Decoded),
      [IndividualWeight = Weight / MatchedOpcodes.size()](unsigned Opcode) {
        return OpcodeHistogramEntry{Opcode, IndividualWeight};
      });

  if (MatchedOpcodes.empty())
    return reportNoMatchesError(IO, OpcodeStr);

  return Result;
}

void YAMLHistogramTraits<OpcodeHistogramDecodedEntry>::normalizeEntry(
    yaml::IO &IO, const DenormEntry &E, SmallVectorImpl<SValue> &RawStrings) {
  auto &Decoded = E.Decoded;
  assert(Decoded.size() == 1 && "Expected entry to contain only a "
                                "single opcode, can't serialize");
  auto &First = Decoded.front();
  const auto &OpCC = getContext(IO).OpCC;
  RawStrings.push_back(std::string(OpCC.name(First.Opcode)));
  RawStrings.push_back(std::to_string(First.Weight));
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
                       std::unordered_map<unsigned, double> &Histogram) {
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
    report_fatal_error("Plugin filled histogram with invalid opcodes", false);

  auto InvalidWeightsChecker = [](auto It) { return It.second < 0; };
  if (std::find_if(Histogram.begin(), Histogram.end(), InvalidWeightsChecker) !=
      Histogram.end())
    report_fatal_error("Plugin filled histogram with negative opcodes weights",
                       false);
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
