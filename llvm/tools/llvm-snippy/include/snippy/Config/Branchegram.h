//===-- Branchegram.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Simulator/Types.h"
#include "snippy/Support/YAMLUtils.h"

namespace llvm {
namespace snippy {

template <typename T, typename MemberPtrT>
const auto &getField(const T &Obj, MemberPtrT &&MemberPtr) {
  return std::invoke(std::forward<MemberPtrT>(MemberPtr), Obj);
}

template <typename T, typename MemberPtrT, typename... ArgsTys>
const auto &getField(const T &Obj, MemberPtrT &&MemberPtr, ArgsTys &&...Args) {
  return getField(std::invoke(std::forward<MemberPtrT>(MemberPtr), Obj),
                  std::forward<ArgsTys>(Args)...);
}

template <typename T> struct NumericRange final {
  std::optional<T> Min;
  std::optional<T> Max;
};

struct Branchegram final {
  static constexpr unsigned DefaultAlignment = 1;
  static constexpr double DefaultLoopRatio = 0.5;
  static constexpr unsigned NConsecutiveLoopsDefault = 0;
  static constexpr unsigned MinNLoopIterDefault = 4;
  static constexpr unsigned MaxNLoopIterDefault = 4;
  static constexpr unsigned MaxLoopDepthDefault = 3;

  struct Depth {
    std::optional<unsigned> If;
    unsigned Loop = MaxLoopDepthDefault;
  };

  struct Distance {
    NumericRange<unsigned> Blocks;
    NumericRange<ProgramCounterType> PC;
  };

  bool PermuteCF = true;
  unsigned Alignment = DefaultAlignment;
  double LoopRatio = DefaultLoopRatio;
  unsigned NConsecutiveLoops = NConsecutiveLoopsDefault;
  NumericRange<unsigned> NLoopIter = {MinNLoopIterDefault, MaxNLoopIterDefault};
  Depth MaxDepth;
  Distance Dist;

  void print(raw_ostream &OS) const;

  NumericRange<unsigned> getBlockDistance() const { return Dist.Blocks; }
  NumericRange<ProgramCounterType> getPCDistance() const { return Dist.PC; }

  bool isPCDistanceRequested() const { return Dist.PC.Min || Dist.PC.Max; }

  unsigned getMaxLoopDepth() const { return MaxDepth.Loop; }

  bool hasMaxIfDepth() const { return MaxDepth.If.has_value(); }

  unsigned getMaxIfDepth() const {
    assert(hasMaxIfDepth());
    return *MaxDepth.If;
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const;
#endif
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::Branchegram);
} // namespace llvm
