//===-- FPRNaNIdentifier.cpp ------------------------------------ *- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/FPRNaNIdentifier.h"

#include <numeric>

namespace llvm {
namespace snippy {

using size_type = NaNIdentifier::size_type;

void NaNIdentifier::markRegisterWithNaNState(MCRegister Reg,
                                             const MCRegisterClass &RegClass,
                                             FPRNaNState State) {
  assert(RegClass.contains(Reg) &&
         "MCRegisterClass doesn't contains passed register");

  auto &StatesMap = ClassToStates[&RegClass];
  // Increment the count for the new register state.
  // Decrement the count for the current register state, if the provided
  // register was previously flagged.
  if (auto It = FPRegsInfo.find(Reg); It != FPRegsInfo.end()) {
    if (auto CurrRegState = It->second.first; CurrRegState != State) {
      StatesMap[State]++;
      StatesMap[CurrRegState]--;
    }
  } else {
    StatesMap[State]++;
  }
  FPRegsInfo[Reg].first = State;
  if (auto &RegClasses = FPRegsInfo[Reg].second; !RegClasses.count(&RegClass))
    FPRegsInfo[Reg].second.insert(&RegClass);
}

void NaNIdentifier::getMCRegClasses(
    ArrayRef<MCRegister> Regs,
    SmallVectorImpl<const MCRegisterClass *> &ClassesToFill) const {
  for (auto &&Reg : Regs)
    llvm::copy(getMCRegClasses(Reg), std::back_inserter(ClassesToFill));
  llvm::sort(ClassesToFill);
  // Erase duplicate register classes
  ClassesToFill.erase(llvm::unique(ClassesToFill));
}

size_type NaNIdentifier::getNaNStateRegistersCount(
    const MCRegisterClass &RegClass, ArrayRef<FPRNaNState> NaNStates) const {
  auto ClassStatesIter = ClassToStates.find(&RegClass);
  if (ClassStatesIter == ClassToStates.end())
    return 0;

  auto StatesCountRange = llvm::make_second_range(llvm::make_filter_range(
      ClassStatesIter->second, [&](auto &&StateWithCount) {
        return llvm::find_if(NaNStates, [&](auto State) {
                 return StateWithCount.first == State;
               }) != NaNStates.end();
      }));
  return std::accumulate(StatesCountRange.begin(), StatesCountRange.end(), 0u);
}

} // namespace snippy
} // namespace llvm
