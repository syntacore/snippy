//===-- BurstGram.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/GeneratorUtils/LLVMState.h"

namespace llvm {
namespace snippy {

void BurstGramData::convertToCustomMode(const OpcodeHistogram &Histogram,
                                        const MCInstrInfo &II) {
  if (Mode == BurstMode::CustomBurst || Mode == BurstMode::Basic)
    return;
  assert(!Groupings &&
         "Groupings are specified but burst mode is not \"custom\"");
  Groupings = BurstGramData::GroupingsTy();
  auto CopyFirstIfSatisfies = [&Histogram](auto &Cont, auto &&Cond) {
    copy_if(make_first_range(Histogram), std::inserter(Cont, Cont.end()), Cond);
  };
  auto Group = BurstGramData::UniqueOpcodesTy{};
  switch (Mode) {
  default:
    llvm_unreachable("Unknown Burst Mode");
  case BurstMode::LoadBurst:
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayLoad() && !II.get(Opc).mayStore();
    });
    break;
  case BurstMode::StoreBurst:
    CopyFirstIfSatisfies(Group,
                         [&II](auto Opc) { return II.get(Opc).mayStore(); });
    break;
  case BurstMode::LoadStoreBurst:
    CopyFirstIfSatisfies(Group,
                         [&II](auto Opc) { return II.get(Opc).mayStore(); });
    Groupings->push_back(Group);
    Group.clear();
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayLoad() && !II.get(Opc).mayStore();
    });
    break;
  case BurstMode::MixedBurst:
    CopyFirstIfSatisfies(Group, [&II](auto Opc) {
      return II.get(Opc).mayStore() || II.get(Opc).mayLoad();
    });
    break;
  }
  Groupings->push_back(Group);
  Mode = BurstMode::CustomBurst;
}

void BurstGramData::removeUnsupportedOpcodes(LLVMState &State,
                                             const OpcodeCache &OpCC) {
  assert(Mode == BurstMode::CustomBurst &&
         "At this point burst mode should be \"custom\"");
  assert(Groupings);
  const auto &Tgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  for (auto &&Group : *Groupings) {
    bool WasEmpty = Group.empty();
    // Can't use std::remove with ordered container
    for (auto Fst = Group.begin(); Fst != Group.end();) {
      if (!Tgt.canUseInBurstMode(InstrInfo.get(*Fst))) {
        snippy::warn(WarningName::BurstMode, State.getCtx(),
                     Twine("Opcode ") + OpCC.name(*Fst) +
                         " is not supported in the burst mode",
                     "generator will generate it but not in a burst group.");
        Fst = Group.erase(Fst);
      } else
        ++Fst;
    }

    if (Group.empty() && !WasEmpty)
      snippy::warn(
          WarningName::BurstMode, State.getCtx(), "Empty burst group",
          "no supported opcodes were specified, this group will be ignored.");
  }
}

} // namespace snippy
} // namespace llvm
