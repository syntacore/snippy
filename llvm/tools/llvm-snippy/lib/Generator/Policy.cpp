//===-- Policy.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Policy.h"
#include "snippy/Generator/GeneratorContext.h"

namespace llvm {
namespace snippy {

DefaultGenPolicy::DefaultGenPolicy(const GeneratorContext &SGCtx,
                                   std::function<bool(unsigned)> Filter,
                                   bool MustHavePrimaryInstrs,
                                   ArrayRef<OpcodeHistogramEntry> Overrides)
    : OpcGen(SGCtx.createFlowOpcodeGenerator(Filter, MustHavePrimaryInstrs,
                                             Overrides)) {}

BurstGenPolicy::BurstGenPolicy(const GeneratorContext &SGCtx,
                               unsigned BurstGroupID) {
  auto &State = SGCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &BGram = SGCtx.getBurstGram();

  assert(BGram.Mode != BurstMode::Basic);
  assert(BGram.Mode == BurstMode::CustomBurst &&
         "At this point burst mode should be \"custom\"");
  assert(BGram.Groupings &&
         "Custom burst mode was specified but groupings are empty");
  const auto &Groupings = BGram.Groupings.value();

  auto BurstGroupId = BurstGroupID;
  assert(BurstGroupId < Groupings.size());
  const auto &Group = Groupings[BurstGroupId];

  std::copy_if(Group.begin(), Group.end(), std::back_inserter(Opcodes),
               [&SnippyTgt, &State,
                &OpcCache = SGCtx.getOpcodeCache()](unsigned Opcode) {
                 if (!SnippyTgt.canUseInMemoryBurstMode(Opcode)) {
                   snippy::warn(
                       WarningName::BurstMode, State.getCtx(),
                       Twine("Opcode ") + OpcCache.name(Opcode) +
                           " is not supported in memory burst mode",
                       "generator will generate it but not in a burst group.");
                   return false;
                 }
                 return true;
               });

  std::vector<double> Weights;
  const auto &Cfg = SGCtx.getConfig();
  auto OpcodeToNumOfGroups = BGram.getOpcodeToNumBurstGroups();
  std::transform(Opcodes.begin(), Opcodes.end(), std::back_inserter(Weights),
                 [&Cfg, &OpcodeToNumOfGroups](unsigned Opcode) {
                   assert(OpcodeToNumOfGroups.count(Opcode));
                   return Cfg.Histogram.weight(Opcode) /
                          OpcodeToNumOfGroups[Opcode];
                 });
  Dist = std::discrete_distribution<size_t>(Weights.begin(), Weights.end());
}
} // namespace snippy
} // namespace llvm
