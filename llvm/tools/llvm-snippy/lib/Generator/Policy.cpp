//===-- Policy.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Policy.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Target/Target.h"
namespace llvm {
namespace snippy {

namespace planning {

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

static std::optional<int>
getOffsetImmediate(ArrayRef<PreselectedOpInfo> Preselected) {
  auto Found =
      find_if(Preselected, [](auto &OpInfo) { return OpInfo.isImm(); });
  if (Found == Preselected.end())
    return std::nullopt;
  auto Imm = Found->getImm();
  assert(Imm.getMax() == Imm.getMin());
  return Imm.getMax();
}

void BurstGenPolicy::initialize(InstructionGenerationContext &InstrGenCtx,
                                const RequestLimit &Limit) {
  assert(Limit.isNumLimit());
  auto &State = InstrGenCtx.GC.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  std::generate_n(std::back_inserter(Instructions), Limit.getLimit(), [this] {
    return InstructionRequest{genOpc(), {}};
  });
  auto IsMemUser = [&Tgt](auto Opc) -> bool {
    return Tgt.countAddrsToGenerate(Opc);
  };
  std::vector<unsigned> MemUsers;
  copy_if(map_range(Instructions, [](auto &&IR) { return IR.Opcode; }),
          std::back_inserter(MemUsers), IsMemUser);
  auto OpcodeIdxToBaseReg = generateBaseRegs(InstrGenCtx.MBB, MemUsers,
                                             InstrGenCtx.RP, InstrGenCtx.GC);

  auto RP = InstrGenCtx.GC.getRegisterPool();
  auto OpcodeIdxToAI =
      mapOpcodeIdxToAI(InstrGenCtx.MBB, OpcodeIdxToBaseReg, MemUsers,
                       InstrGenCtx.Ins, RP, InstrGenCtx.GC);
  unsigned MemUsersIdx = 0;
  for (auto &&Instr : Instructions) {
    const auto &InstrDesc = InstrInfo.get(Instr.Opcode);
    if (IsMemUser(Instr.Opcode)) {
      auto BaseReg = OpcodeIdxToBaseReg[MemUsersIdx];
      auto AI = OpcodeIdxToAI[MemUsersIdx];
      auto Preselected = selectOperands(InstrDesc, BaseReg, AI);
      Instr.Preselected =
          selectConcreteOffsets(InstrDesc, Preselected, InstrGenCtx.GC);
      AddressInfo ActualAI = AI;
      auto Offset = getOffsetImmediate(Instr.Preselected);
      ActualAI.Address += Offset.value_or(0);
      markMemAccessAsUsed(InstrDesc, ActualAI, MemAccessKind::BURST,
                          InstrGenCtx.GC);
      ++MemUsersIdx;
    }
  }
}

} // namespace planning
} // namespace snippy
} // namespace llvm
