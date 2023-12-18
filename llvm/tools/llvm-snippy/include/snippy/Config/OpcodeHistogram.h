//===-- OpcodeHistogram.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/OpcodeCache.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/MC/MCInstrDesc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>

namespace llvm {
namespace snippy {

class SnippyTarget;
class ConfigIOContext;

struct OpcodeHistogramEntry {
  // If Weight set to this value (in fact any negative one) this
  // means that user should ignore the respected entry
  static constexpr double IgnoredWeight = -1.0;
  bool deactivated() const { return Weight < 0.0; }

  unsigned Opcode;
  double Weight;
};

class OpcodeHistogram : private std::unordered_map<unsigned, double> {
public:
  using typename unordered_map::const_iterator;
  using typename unordered_map::iterator;
  using typename unordered_map::value_type;

  using unordered_map::begin;
  using unordered_map::count;
  using unordered_map::end;
  using unordered_map::find;
  using unordered_map::insert;
  using unordered_map::insert_or_assign;
  using unordered_map::size;

  double getOpcodesWeight(std::function<bool(unsigned)> Pred) const {
    return std::accumulate(begin(), end(), 0.0,
                           [&Pred](double Accumulation, auto &&Hist) -> double {
                             if (Pred(Hist.first))
                               return Accumulation + Hist.second;
                             return Accumulation;
                           });
  }

  double getTotalWeight() const {
    return getOpcodesWeight([](unsigned) { return true; });
  }

  double getCFWeight(const OpcodeCache &OpCC) const {
    return getOpcodesWeight([&OpCC](unsigned Opcode) {
      auto *Desc = OpCC.desc(Opcode);
      return Desc && Desc->isBranch();
    });
  }

  bool hasCFInstrs(const OpcodeCache &OpCC) const {
    return std::any_of(begin(), end(), [&OpCC](auto &Hist) {
      auto *Desc = OpCC.desc(Hist.first);
      return Desc && Desc->isBranch();
    });
  }

  bool hasCallInstrs(const OpcodeCache &OpCC, const SnippyTarget &Tgt) const;

  unsigned getCFInstrsNum(unsigned InstrsNum, const OpcodeCache &OpCC) const {
    double CFInstrsWeight =
        std::accumulate(begin(), end(), 0.0,
                        [&OpCC](double Accumulation, auto &&Hist) -> double {
                          auto *Desc = OpCC.desc(Hist.first);
                          if (Desc && Desc->isBranch())
                            return Accumulation + Hist.second;
                          return Accumulation;
                        });

    double TotalWeight = getTotalWeight();

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

  double weight(unsigned Opcode) const {
    auto HIt = find(Opcode);
    if (HIt == end())
      return 0.0;
    return HIt->second;
  }
};

struct OpcodeHistogramDecodedEntry {
  OpcodeHistogramDecodedEntry(StringRef RP = "") : RegexPattern(RP) {}
  OpcodeHistogramDecodedEntry(std::initializer_list<OpcodeHistogramEntry> List)
      : Decoded(List.begin(), List.end()) {}

  // Each histogram entry can correspond to multiple opcode -> weight mappings
  SmallVector<OpcodeHistogramEntry> Decoded;
  std::string RegexPattern;
};

} // namespace snippy

template <>
struct snippy::YAMLHistogramTraits<snippy::OpcodeHistogramDecodedEntry> {
  static ConfigIOContext &getContext(yaml::IO &IO) {
    return *static_cast<ConfigIOContext *>(IO.getContext());
  }

  static OpcodeHistogramDecodedEntry
  reportError(yaml::IO &IO, const Twine &Prefix, const Twine &Msg) {
    IO.setError(Prefix + ": " + Msg);
    return {};
  }

  static OpcodeHistogramDecodedEntry reportNoMatchesError(yaml::IO &IO,
                                                          StringRef OpcodeStr) {
    reportError(IO, "Illegal opcode for specified cpu",
                Twine(OpcodeStr) + "\nUse -list-opcode-names option "
                                   "to check for available instructions!");
    return {};
  }

  using DenormEntry = OpcodeHistogramDecodedEntry;
  using MapType = OpcodeHistogram;

  static DenormEntry denormalizeEntry(yaml::IO &IO, StringRef OpcodeStr,
                                      double Weight);
  static void normalizeEntry(yaml::IO &IO, const DenormEntry &E,
                             SmallVectorImpl<SValue> &RawStrings);

  static MapType denormalizeMap(yaml::IO &IO, ArrayRef<DenormEntry> Entries);
  static void normalizeMap(yaml::IO &IO, const MapType &Hist,
                           std::vector<DenormEntry> &Entries);
  static std::string validate(ArrayRef<DenormEntry> Entries) { return ""; }
};

} // namespace llvm
