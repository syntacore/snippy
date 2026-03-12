//===-- ImmediateHistogram.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/GeneratorUtils/LLVMState.h"
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
  SmallVector<StringRef> Matches;
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
      StringRef Name = OpCC.name(Opc);
      auto Res = RX.match(Name, &Matches);
      if (Res && Matches.size() == 1 && Matches[0] == Name) {
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

namespace yaml {

void MappingTraits<snippy::ImmHistOperands>::mapping(
    IO &Io, snippy::ImmHistOperands &Map) {
  Io.mapRequired("operands", Map.Operands);
}

void yamlize(IO &Io, const snippy::ImmHistOperandsEntryMapMapper &, bool,
             EmptyContext &) {
  Io.setError("Immediate histogram operands entry should be either sequence or "
              "scalar. But map was encountered");
}

void MappingTraits<snippy::ImmediateHistogramSequence>::mapping(
    IO &Io, snippy::ImmediateHistogramSequence &ImmHist) {
  snippy::YAMLHistogramIO<snippy::ImmediateHistogramEntry> ImmHistIO(ImmHist);
  EmptyContext Ctx;
  yamlize(Io, ImmHistIO, false, Ctx);
}

struct ImmHistOperandsEntryNorm final {
  snippy::ImmHistOperandsEntry::Kind EntryKind =
      snippy::ImmHistOperandsEntry::Kind::Sequence;
  snippy::ImmediateHistogramSequence Seq;
};

struct ImmHistOperandsEntryNormalization final {
  ImmHistOperandsEntryNorm Data;

  ImmHistOperandsEntryNormalization(IO &Io) {}

  ImmHistOperandsEntryNormalization(
      IO &Io, const snippy::ImmHistOperandsEntry &Denorm) {
    Data.EntryKind = Denorm.getKind();
    if (Denorm.isSequence())
      Data.Seq = Denorm.getSequence();
  }

  snippy::ImmHistOperandsEntry denormalize(IO &Io) {
    if (Data.EntryKind == snippy::ImmHistOperandsEntry::Kind::Sequence)
      return snippy::ImmHistOperandsEntry(Data.Seq);
    if (Data.EntryKind == snippy::ImmHistOperandsEntry::Kind::Uniform)
      return snippy::ImmHistOperandsEntry();
    llvm_unreachable("Unrecognized ImmHistOperandsEntry kind");
  }
};

template <> struct PolymorphicTraits<ImmHistOperandsEntryNorm> {
  static NodeKind getKind(const ImmHistOperandsEntryNorm &Norm) {
    if (Norm.EntryKind == snippy::ImmHistOperandsEntry::Kind::Uniform)
      return NodeKind::Scalar;
    if (Norm.EntryKind == snippy::ImmHistOperandsEntry::Kind::Sequence)
      return NodeKind::Sequence;
    llvm_unreachable("Unrecognized ImmHistOperandsEntry kind");
  }

  static snippy::ImmHistOperandsEntry::Kind &
  getAsScalar(ImmHistOperandsEntryNorm &Norm) {
    return Norm.EntryKind;
  }

  static snippy::ImmediateHistogramSequence &
  getAsSequence(ImmHistOperandsEntryNorm &Norm) {
    return Norm.Seq;
  }

  static snippy::ImmHistOperandsEntryMapMapper
  getAsMap(ImmHistOperandsEntryNorm &) {
    return snippy::ImmHistOperandsEntryMapMapper();
  }
};

template <> struct ScalarEnumerationTraits<snippy::ImmHistOperandsEntry::Kind> {
  static void enumeration(IO &Io,
                          snippy::ImmHistOperandsEntry::Kind &EntryKind) {
    Io.enumCase(EntryKind, "uniform",
                snippy::ImmHistOperandsEntry::Kind::Uniform);
  }
};

void MappingTraits<snippy::ImmHistOperandsEntryHolder>::mapping(
    IO &Io, snippy::ImmHistOperandsEntryHolder &EntryHolder) {
  MappingNormalization<ImmHistOperandsEntryNormalization,
                       snippy::ImmHistOperandsEntry>
      Norm(Io, EntryHolder.Underlying);
  EmptyContext Ctx;
  yamlize(Io, Norm->Data, false, Ctx);
}

} // namespace yaml
} // namespace llvm
