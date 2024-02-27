//===-- BlockGenPlan.cpp-----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/BlockGenPlan.h"
#include "snippy/Support/RandUtil.h"

namespace llvm {
namespace snippy {

// The algorithm that creates generation plan does not randomize it and only
// adds packs that are necessary to generate. So, generation plan for a basic
// block might look like the following:
//   Format: { PackType, InstCount }
//           { Burst, 5 }
//           { Burst, 2 }
//           { Burst, 3 }
//           { Plain, 6 }
//           { Plain, 7 }
// We want to randomize it:
//   * plain instructions must be scattered over the whole plan. Plain packs may
//   be joint and split.
//   * there is no need to somehow reorder burst groups because they were
//   randomly added to the plan. Also, by keeping the order of burst groups we
//   guarantee that only the last burst group might be underfilled (what if we
//   must generate 15 instructions in burst groups, but burst group size is
//   10?).
// After randomization original plan may be changed to:
//           { Plain, 1 }
//           { Burst, 5 }
//           { Burst, 2 }
//           { Plain, 7 }
//           { Burst, 3 }
//           { Plain, 5 }
void SingleBlockGenPlanTy::randomize() {
  if (Packs.empty())
    return;

  // We request all plain packs to be after burst packs.
  auto InstPackIt =
      std::find_if(Packs.begin(), Packs.end(),
                   [](const auto &Pack) { return Pack.isPlain(); });
  if (InstPackIt == Packs.end())
    return;

  assert(std::all_of(InstPackIt, Packs.end(),
                     [](const auto &Pack) { return Pack.isPlain(); }) &&
         "Before randomization all 'plain' packs must be at the end.");

  // Calculate the number of instructions in all plain packs. Since plain packs
  // may differ only by instruction count, we can think that only one plain pack
  // exists with instcount of PlainPackSize.
  auto PlainPackSize = std::accumulate(
      InstPackIt, Packs.end(), 0ull,
      [](auto Acc, const auto &Pack) { return Pack.getLimit() + Acc; });

  // At this point let's say that we have `PlainPackSize` packs of plain
  // instructions each of size one, e.g.:
  //   P_0  P_1  P_2  P_3  P_4  P_5
  // Let's also say that we have N burst packs:
  //   B_0  B_1  B_2
  // We generate N indices in [0, PlainPackSize - 1] for plain packs. By these
  // indices plain packs will be mixed with burst packs:
  //   Idxs: 0, 1, 1, 4 (generate 3 idxs as there are three burst packs).
  //   Plain packs with idxs [0, 1) will be added before the first burst pack
  //   Plain packs with idxs [1, 1) (<--- no packs) will be added before the
  //   second burst pack and so on. If the last index is smaller than
  //   PlainPackSize than all plain packs left will be added after the last
  //   burst pack (in our case they are P_4 and P_5).
  // If we add more than one plain pack, then they'll be joint into one.
  // Resulting sequence of packs for our example is:
  //   { Plain, 1 } -- P_0
  //   { B_0 }
  //   { B_1 }
  //   { Plain, 3 } -- P_1  P_2  P_3
  //   { B_2 }
  //   { Plain, 2 } -- P_4  P_5
  std::vector<size_t> Idxs;
  Idxs.push_back(0);
  std::generate_n(
      std::back_inserter(Idxs), std::distance(Packs.begin(), InstPackIt),
      [PlainPackSize] { return RandEngine::genInRange(0ull, PlainPackSize); });
  std::sort(Idxs.begin(), Idxs.end());

  auto NonPlainPacksIt = Packs.begin();
  std::vector<InstPackTy> NewPacks;
  for (auto [FirstIdx, SecondIdx] : zip(Idxs, drop_begin(Idxs))) {
    assert(FirstIdx <= SecondIdx);
    if (FirstIdx != SecondIdx)
      NewPacks.emplace_back(SecondIdx - FirstIdx);

    assert(!NonPlainPacksIt->isPlain());
    NewPacks.push_back(*NonPlainPacksIt);
    assert(NonPlainPacksIt != Packs.end());
    ++NonPlainPacksIt;
  }

  if (Idxs.back() != PlainPackSize)
    NewPacks.emplace_back(PlainPackSize - Idxs.back());

  std::swap(NewPacks, Packs);
}

} // namespace snippy
} // namespace llvm
