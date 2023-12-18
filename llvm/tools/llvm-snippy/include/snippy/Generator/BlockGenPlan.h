//===-- BlockGenPlan.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GeneratorContext.h"

namespace llvm {
namespace snippy {

class InstPackTy final {
private:
  size_t GenerationLimit;
  // Burst group ID from BurstGramData::Groupings
  std::optional<size_t> GroupId;

public:
  InstPackTy(size_t LimitIn, std::optional<size_t> GroupIdIn = std::nullopt)
      : GenerationLimit(LimitIn), GroupId(GroupIdIn) {}

  bool isBurst() const { return GroupId.has_value(); }
  bool isPlain() const { return !isBurst(); }
  size_t getLimit() const { return GenerationLimit; }

  size_t getGroupId() const {
    assert(isBurst());
    return *GroupId;
  }
};

class SingleBlockGenPlanTy {
  GenerationMode GM;
  std::vector<InstPackTy> Packs;

public:
  SingleBlockGenPlanTy(GenerationMode Mode) : GM(Mode), Packs() {}

  void add(InstPackTy Pack) {
    assert((GM != GenerationMode::Size || Pack.isPlain()) &&
           "Only plain packs supported for size generation");
    Packs.push_back(std::move(Pack));
  }

  GenerationMode genMode() const noexcept { return GM; }

  void randomize();

  std::optional<size_t> limit(GenerationMode Mode) const {
    if (Mode != GM)
      return std::nullopt;

    return std::accumulate(
        Packs.begin(), Packs.end(), 0ull,
        [](auto Init, auto Pack) { return Init + Pack.getLimit(); });
  }

  size_t instrCount() const {
    assert(GM == GenerationMode::NumInstrs);
    auto Limit = limit(GM);
    assert(Limit.has_value());
    return Limit.value();
  }

  size_t size() const {
    assert(GM == GenerationMode::Size);
    auto Limit = limit(GM);
    assert(Limit.has_value());
    return Limit.value();
  }

  const std::vector<InstPackTy> &packs() const { return Packs; }
};

using BlocksGenPlanTy =
    std::map<const MachineBasicBlock *, SingleBlockGenPlanTy, MIRComp>;

} // namespace snippy
} // namespace llvm
