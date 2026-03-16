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
#include "snippy/Generator/OperandsReinitialiazationPolicy.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Support/Error.h"
#include "snippy/Target/Target.h"
#include "llvm/MC/MCInstrDesc.h"

#include <random>

namespace llvm {
namespace snippy {

namespace planning {

static std::unique_ptr<FloatSemanticsSamplerHolder>
createFloatSemanticsSampler(const CommonPolicyConfig &Cfg) {
  return std::make_unique<FloatSemanticsSamplerHolder>(Cfg.FPUConfig.Overwrite);
}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    SnippyProgramContext &ProgCtx, const SimulatorContext &SimCtx)
    : MBB(MBB), Ins(Ins), ProgCtx(ProgCtx), SimCtx(SimCtx),
      NaNIdent(ProgCtx.getLLVMState().getSnippyTarget().getFPRegsCount(
          MBB.getParent()->getSubtarget())),
      RPS(ProgCtx) {
  switchConfig();
}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    SnippyProgramContext &ProgCtx)
    : NullSimCtx(std::make_unique<SimulatorContext>()), MBB(MBB), Ins(Ins),
      ProgCtx(ProgCtx), SimCtx(*NullSimCtx),
      NaNIdent(ProgCtx.getLLVMState().getSnippyTarget().getFPRegsCount(
          MBB.getParent()->getSubtarget())),
      RPS(ProgCtx) {
  switchConfig();
}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    GeneratorContext &GC, const SimulatorContext &SimCtx)
    : InstructionGenerationContext(MBB, Ins, GC.getProgramContext(), SimCtx) {
  append(&GC.getMemoryAccessSampler());
  switchConfig(*GC.getConfig().CommonPolicyCfg);
}
InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    GeneratorContext &GC)
    : InstructionGenerationContext(MBB, Ins, GC.getProgramContext()) {
  append(&GC.getMemoryAccessSampler());
  switchConfig(*GC.getConfig().CommonPolicyCfg);
}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    GeneratorContext &GC, RegPoolWrapper &RPW)
    : NullSimCtx(std::make_unique<SimulatorContext>()), MBB(MBB), Ins(Ins),
      ProgCtx(GC.getProgramContext()), SimCtx(*NullSimCtx),
      NaNIdent(ProgCtx.getLLVMState().getSnippyTarget().getFPRegsCount(
          MBB.getParent()->getSubtarget())),
      RPS(ProgCtx, RPW) {
  append(&GC.getMemoryAccessSampler());
  switchConfig(*GC.getConfig().CommonPolicyCfg);
}

InstructionGenerationContext::~InstructionGenerationContext() = default;

IAPIntSampler &
InstructionGenerationContext::getOrCreateFloatOverwriteValueSampler(
    const fltSemantics &Semantics) {
  const auto &Cfg = getCommonCfg();
  // lazy construction.
  if (!FloatOverwriteSamplers)
    FloatOverwriteSamplers = createFloatSemanticsSampler(Cfg);
  assert(FloatOverwriteSamplers.get());
  auto SamplerRefOrErr = FloatOverwriteSamplers->getSamplerFor(Semantics);
  if (!SamplerRefOrErr)
    snippy::fatal(ProgCtx.getLLVMState().getCtx(), "Internal error",
                  SamplerRefOrErr.takeError());
  return *SamplerRefOrErr;
}

DefaultGenPolicy::DefaultGenPolicy(
    SnippyProgramContext &ProgCtx, const DefaultPolicyConfig &Cfg,
    const ModeChangingInstPolicy *ModeChangingPolicy)
    : OpcGen(nullptr), Cfg(&Cfg), ModeChangingPolicy(ModeChangingPolicy) {
  assert(!Cfg.isApplyValuegramEachInstr() &&
         "In this case you must use ValuegramGenPolicy");
}

BurstGenPolicy::BurstGenPolicy(SnippyProgramContext &ProgCtx,
                               const BurstPolicyConfig &Cfg,
                               unsigned BurstGroupID)
    : Cfg(&Cfg) {
  const auto &BGram = Cfg.Burst;

  assert(BGram.Mode != BurstMode::Basic);
  assert(BGram.Mode == BurstMode::CustomBurst &&
         "At this point burst mode should be \"custom\"");
  assert(BGram.Groupings &&
         "Custom burst mode was specified but groupings are empty");
  const auto &Groupings = BGram.Groupings.value();

  auto BurstGroupId = BurstGroupID;
  assert(BurstGroupId < Groupings.size());
  const auto &Group = Groupings[BurstGroupId];

  std::copy(Group.begin(), Group.end(), std::back_inserter(Opcodes));

  std::vector<double> Weights;
  auto OpcodeToNumOfGroups = BGram.getOpcodeToNumBurstGroups();
  std::transform(Opcodes.begin(), Opcodes.end(), std::back_inserter(Weights),
                 [&Cfg, &OpcodeToNumOfGroups](unsigned Opcode) {
                   assert(OpcodeToNumOfGroups.count(Opcode));
                   return Cfg.BurstOpcodeWeights.at(Opcode) /
                          OpcodeToNumOfGroups[Opcode];
                 });
  Dist = std::discrete_distribution<size_t>(Weights.begin(), Weights.end());
}

void DefaultGenPolicy::initialize(InstructionGenerationContext &InstrGenCtx,
                                  const RequestLimit &Limit) {
  InstrGenCtx.switchConfig(*Cfg);

  if (Limit.isEmpty())
    return;

  const auto &Tgt = InstrGenCtx.ProgCtx.getLLVMState().getSnippyTarget();
  const auto &Filter = ModeChangingPolicy
                           ? ModeChangingPolicy->getOpcodeFilter()
                           : getDefaultFilter(Tgt);
  auto Err = Cfg->createOpcodeGenerator(Filter).moveInto(OpcGen);
  if (Err)
    snippy::fatal(
        Twine("Failed to create OpcodeGenerator in DefaultGenPolicy: ") +
        toString(std::move(Err)));
}

void BurstGenPolicy::initialize(InstructionGenerationContext &InstrGenCtx,
                                const RequestLimit &Limit) {
  InstrGenCtx.switchConfig(*Cfg);
  assert(Limit.isNumLimit());
  auto &State = InstrGenCtx.ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  std::generate_n(std::back_inserter(Instructions), Limit.getLimit(),
                  [this] { return InstructionRequest{genOpc(), {}}; });
  auto IsMemUser = [&Tgt](auto Opc) -> bool {
    return Tgt.countAddrsToGenerate(Opc);
  };
  std::vector<unsigned> MemUsers;
  MemUsers.reserve(Instructions.size());
  copy_if(map_range(Instructions, [](auto &&IR) { return IR.Opcode; }),
          std::back_inserter(MemUsers), IsMemUser);
  auto RP = InstrGenCtx.pushRegPool();
  auto [MemUserIdxToPreselectedOps, RegsToInit] =
      selectOperandsForMemoryInstructions(InstrGenCtx, MemUsers, *RP);
  // Here we collected all registers that should be initialized (we don't
  // initialize registers for non-memory instructions). Initialize them all in
  // one go.
  initializeBaseRegs(InstrGenCtx, RegsToInit);
  unsigned MemUsersIdx = 0;
  for (auto &&Instr : Instructions) {
    if (IsMemUser(Instr.Opcode)) {
      Instr.Preselected = MemUserIdxToPreselectedOps[MemUsersIdx++];
    } else {
      // For instructions that do not use memory we can simply preselect their
      // operands.
      auto &II = State.getInstrInfo();
      auto &InstrDesc = II.get(Instr.Opcode);
      Instr.Preselected.resize(InstrDesc.getNumOperands());
      // To avoid spoling registers used in memory instruction we use same
      // register pool and mark all initialized registers as excluded
      DenseSet<Register> Excluded;
      Excluded.insert_range(make_first_range(RegsToInit));
      selectNonMemoryOperands(InstrDesc, Instr.Preselected, InstrGenCtx, *RP,
                              Excluded);
    }
  }
}

LLVMState &InstructionGenerationContext::getLLVMStateImpl() const {
  return ProgCtx.getLLVMState();
}

static std::unique_ptr<IOperandsReinitializationValueSource>
getValuegramPolicyValueSource(const DefaultPolicyConfig &Cfg) {
  if (Cfg.OperandsReinitialization) {
    assert(!Cfg.Valuegram.has_value() &&
           "Specifying operands-reinitialization with valuegram-operands-regs "
           "is prohibited");
    return std::make_unique<OperandsReinitializationOpcodeValuegramSource>(
        Cfg.OpcodeToORSettingsMap);
  }
  if (Cfg.Valuegram) {
    const auto &RegsHistograms = Cfg.Valuegram->RegsHistograms;
    return std::make_unique<OperandsReinitializationValuegramSource>(
        RegsHistograms);
  }
  llvm_unreachable("Unrecognized operands reinitialization policy");
}

void ModeChangingInstPolicy::initialize(
    InstructionGenerationContext &InstrGenCtx, const RequestLimit &Limit) {
  assert(!OpcodeFilter && "Opcode filter should not be created at this point.");

  const auto &Tgt = InstrGenCtx.ProgCtx.getLLVMState().getSnippyTarget();
  OpcodeFilter = Tgt.generateModeChangeAndGetFilter(InstrGenCtx, MetadataMark);
}

GenPolicy createGenPolicy(SnippyProgramContext &ProgCtx,
                          const DefaultPolicyConfig &Cfg,
                          const ModeChangingInstPolicy *ModeChangingPolicy) {
  if (Cfg.isApplyValuegramEachInstr()) {
    assert(Cfg.Valuegram.has_value() ||
           Cfg.OperandsReinitialization.has_value());
    auto ValuegramValueSource = getValuegramPolicyValueSource(Cfg);
    return planning::ValuegramGenPolicy(
        ProgCtx, Cfg, std::move(ValuegramValueSource), ModeChangingPolicy);
  }
  return planning::DefaultGenPolicy(ProgCtx, Cfg, ModeChangingPolicy);
}

} // namespace planning
} // namespace snippy
} // namespace llvm
