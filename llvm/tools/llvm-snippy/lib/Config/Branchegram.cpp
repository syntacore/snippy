//===-- Branchegram.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Branchegram.h"

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
void yaml::MappingTraits<Branchegram>::mapping(yaml::IO &IO,
                                               Branchegram &Branches) {
  IO.mapOptional("permutation", Branches.PermuteCF);
  IO.mapOptional("alignment", Branches.Alignment);
  IO.mapOptional("consecutive-loops", Branches.NConsecutiveLoops);
  IO.mapOptional("loop-ratio", Branches.LoopRatio);
  IO.mapOptional("number-of-loop-iterations", Branches.NLoopIter);
  IO.mapOptional("max-depth", Branches.MaxDepth);
  IO.mapOptional("distance", Branches.Dist);
}

std::string yaml::MappingTraits<Branchegram>::validate(yaml::IO &IO,
                                                       Branchegram &Branches) {
  if ((Branches.LoopRatio < 0.0 || Branches.LoopRatio > 1.0))
    return std::string("Loop ratio expected to be >= 0 and <= 1");

  if (!isPowerOf2_64(Branches.Alignment))
    return std::string("Alignment expected to be a power of 2");

  if (Branches.NLoopIter.Min == 0)
    return std::string("Number of loop iterations must be > 0");

  if (Branches.NConsecutiveLoops != 0) {
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

template <typename T> struct yaml::MappingTraits<NumericRange<T>> {
  static void mapping(yaml::IO &IO, NumericRange<T> &Range) {
    IO.mapOptional("min", Range.Min);
    IO.mapOptional("max", Range.Max);
  }

  static std::string validate(yaml::IO &IO, NumericRange<T> &Range) {
    return (!Range.Min || !Range.Max || *Range.Min <= *Range.Max)
               ? std::string()
               : std::string("Min expected to be less or equal max");
  }
};

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
