//===-- ImmediateHistogram.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Support/YAMLHistogram.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "snippy-opcode-to-immediate-histogram-map"

namespace llvm {
namespace snippy {

template <> struct YAMLHistogramTraits<ImmediateHistogramEntry> {
  using DenormEntry = ImmediateHistogramEntry;
  using MapType = ImmediateHistogramSequence;

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef ParseStr,
                                      double Weight) {
    int Value = 0;
    auto ParseFailed = ParseStr.getAsInteger(0, Value);
    if (ParseFailed) {
      Io.setError("Immediate histogram entry value " + ParseStr +
                  " is not an integer");
    }
    return {Value, Weight};
  }

  static void normalizeEntry(yaml::IO &Io, const DenormEntry &E,
                             SmallVectorImpl<SValue> &RawStrings) {
    RawStrings.push_back(std::to_string(E.Value));
    RawStrings.push_back(std::to_string(E.Weight));
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> Entries) {
    MapType Hist;
    SmallVector<DenormEntry> CopiedEntries(Entries);
    sort(CopiedEntries, [](DenormEntry LHS, DenormEntry RHS) {
      return LHS.Value < RHS.Value;
    });
    transform(CopiedEntries, std::back_inserter(Hist.Values),
              [](DenormEntry E) { return E.Value; });
    transform(CopiedEntries, std::back_inserter(Hist.Weights),
              [](DenormEntry E) { return E.Weight; });
    return Hist;
  }

  static void normalizeMap(yaml::IO &Io, const MapType &Hist,
                           std::vector<DenormEntry> &Entries) {
    transform(zip(Hist.Values, Hist.Weights), std::back_inserter(Entries),
              [](auto &&P) {
                auto &&[Value, Weight] = P;
                return DenormEntry{Value, Weight};
              });
  }

  static std::string validate(ArrayRef<DenormEntry> Entries) { return ""; }
};

OpcodeToImmHistSequenceMap::OpcodeToImmHistSequenceMap(
    const ImmediateHistogramRegEx &ImmHist, const OpcodeHistogram &OpcHist,
    const OpcodeCache &OpCC) {
  unsigned NMatched = 0;
  for (auto Opc : make_first_range(OpcHist)) {
    for (auto &&Conf : ImmHist.Exprs) {
      if (NMatched == OpcHist.size()) {
        LLVMContext Ctx;
        snippy::warn(WarningName::InconsistentOptions, Ctx,
                     "Unused regex in immediate histogram",
                     "all opcodes were already matched so no opcodes remained "
                     "to be matched by \"" +
                         Twine(Conf.Expr) + "\".");
        break;
      }
      Regex RX(Conf.Expr);
      auto Name = OpCC.name(Opc);
      if (RX.match(Name)) {
        auto Inserted = Data.emplace(Opc, Conf.Data);
        if (Inserted.second) {
          ++NMatched;
          LLVM_DEBUG(dbgs() << "Immediate Histogram matched opcode \"" << Name
                            << "\" with regex \"" << Conf.Expr << "\".\n");
        }
      }
    }
    // Uniform by default.
    auto Inserted = Data.emplace(Opc, ImmHistOpcodeSettings());
    if (Inserted.second) {
      LLVMContext Ctx;
      snippy::notice(WarningName::NotAWarning, Ctx,
                     "No regex that matches \"" + Twine(OpCC.name(Opc)) +
                         "\" was found in immediate histogram",
                     "Uniform destribution will be used.");
    }
  }
}

} // namespace snippy

LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::ImmediateHistogramEntry)

void yaml::MappingTraits<snippy::ImmediateHistogramSequence>::mapping(
    IO &Io, snippy::ImmediateHistogramSequence &ImmHist) {
  snippy::YAMLHistogramIO<snippy::ImmediateHistogramEntry> ImmHistIO(ImmHist);
  EmptyContext Ctx;
  yamlize(Io, ImmHistIO, false, Ctx);
}

} // namespace llvm
