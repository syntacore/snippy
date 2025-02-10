//===-- Branchegram.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Branchegram.h"

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/YAMLNumericRange.h"

#include "llvm/Support/YAMLTraits.h"
namespace llvm {
namespace snippy {

struct BranchegramOutputWrapper final {
  Branchegram &Branches;
};

} // namespace snippy

template <> struct yaml::MappingTraits<snippy::BranchegramOutputWrapper> {
  static void mapping(yaml::IO &IO, snippy::BranchegramOutputWrapper &BOW) {
    IO.mapRequired("branches", BOW.Branches);
  }
};

namespace snippy {

void Branchegram::print(raw_ostream &OS) const {
  BranchegramOutputWrapper BOW{const_cast<Branchegram &>(*this)};
  outputYAMLToStream(BOW, OS);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void Branchegram::dump() const { print(dbgs()); }
#endif

} // namespace snippy

using namespace snippy;
template <> struct yaml::ScalarTraits<Branchegram::ConsecutiveLoops> {
  using ConsLoopsMode = Branchegram::ConsecutiveLoops::Mode;
  static void output(const Branchegram::ConsecutiveLoops &ConsLoops, void *,
                     llvm::raw_ostream &Out) {
    switch (ConsLoops.M) {
    case ConsLoopsMode::NoConsecutiveLoops:
      Out << "none";
      return;
    case ConsLoopsMode::SomeConsecutiveLoops:
      Out << ConsLoops.N;
      return;
    case ConsLoopsMode::OnlyConsecutiveLoops:
      Out << "all";
      return;
    }
    llvm_unreachable("Unknown consecutive loops mode");
  }

  static StringRef input(StringRef Input, void *,
                         Branchegram::ConsecutiveLoops &ConsLoops) {
    auto MatchStringVal = [Input, &ConsLoops](StringRef ValToMatch,
                                              ConsLoopsMode M) {
      if (Input != ValToMatch)
        return false;

      ConsLoops.M = M;
      return true;
    };

    if (MatchStringVal("all", ConsLoopsMode::OnlyConsecutiveLoops))
      return {};
    if (MatchStringVal("none", ConsLoopsMode::NoConsecutiveLoops))
      return {};
    if (MatchStringVal("off", ConsLoopsMode::NoConsecutiveLoops))
      return {};
    if (MatchStringVal("false", ConsLoopsMode::NoConsecutiveLoops))
      return {};

    if (!to_integer(Input, ConsLoops.N))
      return "invalid number";

    ConsLoops.M = ConsLoops.N == 0 ? ConsLoopsMode::NoConsecutiveLoops
                                   : ConsLoopsMode::SomeConsecutiveLoops;

    return {};
  }

  static QuotingType mustQuote(StringRef) { return QuotingType::None; }
};

using LoopCountersInfo = snippy::Branchegram::LoopCountersInfo;
using OptRange = LoopCountersInfo::OptRange;

// Support structure for mapping
struct LoopCountersSupportMap {
  snippy::NumericRange<unsigned> Range;
  std::optional<bool> IsEnabled;

  bool isOptionRequested() const {
    bool CheckEnabled = IsEnabled.has_value() && IsEnabled.value();
    return CheckEnabled || (!IsEnabled.has_value() && Range.isMinOrMaxSet());
  }
};

template <> struct yaml::MappingTraits<LoopCountersSupportMap> {

  struct NormalizedGroupings {
    LoopCountersSupportMap InfoMap;

    NormalizedGroupings(yaml::IO &) {}

    NormalizedGroupings(yaml::IO &IO, const OptRange &Denorm) {
      if (Denorm.has_value()) {
        if (Denorm->isMinOrMaxSet())
          InfoMap.Range = Denorm.value();
        else
          InfoMap.IsEnabled = true;
      } else {
        InfoMap.IsEnabled = false;
      }
    }

    OptRange denormalize(yaml::IO &IO) {
      if (InfoMap.isOptionRequested())
        return {InfoMap.Range};
      return {};
    }
  };

  static void mapping(yaml::IO &IO, LoopCountersSupportMap &InfoMap) {
    IO.mapOptional("enabled", InfoMap.IsEnabled);
    IO.mapOptional("min", InfoMap.Range.Min);
    IO.mapOptional("max", InfoMap.Range.Max);
  }

  static std::string validate(yaml::IO &IO,
                              LoopCountersSupportMap &LoopCountInfo) {
    if (!IO.outputting() && !LoopCountInfo.IsEnabled &&
        !LoopCountInfo.Range.isMinOrMaxSet())
      return std::string("loop-counters: random-init option requires at least "
                         "one of the following attributes: enabled, min, max. "
                         "But none of them was provided.");

    auto MinOpt = LoopCountInfo.Range.Min;
    auto MaxOpt = LoopCountInfo.Range.Max;
    if (MinOpt && MaxOpt && MinOpt.value() > MaxOpt.value())
      return std::string("'min' expected to be less or equal 'max'");

    return std::string("");
  }
};

template <> struct yaml::MappingTraits<LoopCountersInfo> {
  static void mapping(yaml::IO &IO, LoopCountersInfo &LoopMap) {
    yaml::MappingNormalization<
        yaml::MappingTraits<LoopCountersSupportMap>::NormalizedGroupings,
        OptRange>
        InitRangeMap(IO, LoopMap.InitRange);
    IO.mapOptional("random-init", InitRangeMap->InfoMap);
  }
};

void yaml::MappingTraits<Branchegram>::mapping(yaml::IO &IO,
                                               Branchegram &Branches) {
  IO.mapOptional("permutation", Branches.PermuteCF);
  IO.mapOptional("alignment", Branches.Alignment);
  IO.mapOptional("consecutive-loops", Branches.ConsLoops);
  IO.mapOptional("loop-ratio", Branches.LoopRatio);
  IO.mapOptional("number-of-loop-iterations", Branches.NLoopIter);
  IO.mapOptional("max-depth", Branches.MaxDepth);
  IO.mapOptional("distance", Branches.Dist);
  IO.mapOptional("loop-counters", Branches.LoopCounters);
}

std::string yaml::MappingTraits<Branchegram>::validate(yaml::IO &IO,
                                                       Branchegram &Branches) {
  if ((Branches.LoopRatio < 0.0 || Branches.LoopRatio > 1.0))
    return std::string("Loop ratio expected to be >= 0 and <= 1");

  if (!isPowerOf2_64(Branches.Alignment))
    return std::string("Alignment expected to be a power of 2");

  if (Branches.NLoopIter.Min == 0)
    return std::string("Number of loop iterations must be > 0");

  if (Branches.anyConsecutiveLoops()) {
    if (Branches.LoopRatio != 1.0)
      return std::string(
          "Consecutive loop generation is only supported with loop ratio == 1");
    if (Branches.getMaxLoopDepth() > 1)
      return std::string(
          "Consecutive loop generation is not supported for nested loops");
    if (Branches.getBlockDistance().Min.value_or(0) != 0 ||
        Branches.getBlockDistance().Max.value_or(0) != 0)
      return std::string(
          "Consecutive loop generation is only supported for one-block loops");
  }

  return std::string();
}

template <> struct yaml::MappingTraits<Branchegram::Depth> {
  static void mapping(yaml::IO &IO, Branchegram::Depth &Depth) {
    IO.mapOptional("if", Depth.If);
    IO.mapOptional("loop", Depth.Loop);
  }
};

template <> struct yaml::MappingTraits<Branchegram::Distance> {
  static void mapping(yaml::IO &IO, Branchegram::Distance &Dist) {
    IO.mapOptional("blocks", Dist.Blocks);
    IO.mapOptional("pc", Dist.PC);
  }
};

} // namespace llvm
