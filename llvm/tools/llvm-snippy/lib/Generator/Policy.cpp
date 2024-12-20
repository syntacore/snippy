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
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Target/Target.h"

#include <random>

namespace llvm {
namespace snippy {

namespace planning {

static std::unique_ptr<FloatSemanticsSamplerHolder>
createFloatSemanticsSampler(const GeneratorSettings &GenSettings) {
  if (const auto &FPUConfig = GenSettings.Cfg.FPUConfig;
      FPUConfig.has_value() && FPUConfig->Overwrite.has_value())
    return std::make_unique<FloatSemanticsSamplerHolder>(*FPUConfig->Overwrite);
  return nullptr;
}
InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    SnippyProgramContext &ProgCtx, const GeneratorSettings &GenSettings,
    const SimulatorContext &SimCtx)
    : MBB(MBB), Ins(Ins), ProgCtx(ProgCtx), GenSettings(GenSettings),
      SimCtx(SimCtx), RPS(ProgCtx) {}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    SnippyProgramContext &ProgCtx, const GeneratorSettings &GenSettings)
    : NullSimCtx(std::make_unique<SimulatorContext>()), MBB(MBB), Ins(Ins),
      ProgCtx(ProgCtx), GenSettings(GenSettings), SimCtx(*NullSimCtx),
      RPS(ProgCtx) {}
InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    GeneratorContext &GC, const SimulatorContext &SimCtx)
    : InstructionGenerationContext(MBB, Ins, GC.getProgramContext(),
                                   GC.getGenSettings(), SimCtx) {
  append(&GC.getMemoryAccessSampler());
}

InstructionGenerationContext::InstructionGenerationContext(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
    GeneratorContext &GC)
    : InstructionGenerationContext(MBB, Ins, GC.getProgramContext(),
                                   GC.getGenSettings()) {
  append(&GC.getMemoryAccessSampler());
}
InstructionGenerationContext::~InstructionGenerationContext() = default;

IAPIntSampler &
InstructionGenerationContext::getOrCreateFloatOverwriteValueSampler(
    const fltSemantics &Semantics) {
  auto &FPUCfg = GenSettings.Cfg.FPUConfig;
  assert(FPUCfg && FPUCfg->Overwrite);
  // lazy construction.
  if (!FloatOverwriteSamplers)
    FloatOverwriteSamplers = createFloatSemanticsSampler(GenSettings);
  assert(FloatOverwriteSamplers.get());
  auto SamplerRefOrErr = FloatOverwriteSamplers->getSamplerFor(Semantics);
  if (auto Err = SamplerRefOrErr.takeError())
    snippy::fatal(ProgCtx.getLLVMState().getCtx(), "Internal error",
                  std::move(Err));
  return *SamplerRefOrErr;
}

DefaultGenPolicy::DefaultGenPolicy(
    SnippyProgramContext &ProgCtx, const GeneratorSettings &GenSettings,
    std::function<bool(unsigned)> Filter, bool MustHavePrimaryInstrs,
    ArrayRef<OpcodeHistogramEntry> Overrides,
    const std::unordered_map<unsigned, double> &WeightOverrides = {})
    : OpcGen(GenSettings.createFlowOpcodeGenerator(
          ProgCtx.getOpcodeCache(), Filter, MustHavePrimaryInstrs, Overrides,
          WeightOverrides)) {
  assert(!GenSettings.isApplyValuegramEachInstr() &&
         "In this case you must use ValuegramGenPolicy");
}

ValuegramGenPolicy::ValuegramGenPolicy(
    SnippyProgramContext &ProgCtx, const GeneratorSettings &GenSettings,
    std::function<bool(unsigned)> Filter, bool MustHavePrimaryInstrs,
    ArrayRef<OpcodeHistogramEntry> Overrides,
    const std::unordered_map<unsigned, double> &WeightOverrides = {})
    : OpcGen(GenSettings.createFlowOpcodeGenerator(
          ProgCtx.getOpcodeCache(), Filter, MustHavePrimaryInstrs, Overrides,
          WeightOverrides)) {
  assert(GenSettings.isApplyValuegramEachInstr() &&
         "This policy can only be used when the "
         "-valuegram-operands-regs file provided");
}

std::vector<InstructionRequest>
ValuegramGenPolicy::generateOneInstrWithInitRegs(
    InstructionGenerationContext &InstrGenCtx, unsigned Opcode) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  std::vector<InstructionRequest> InstrWithInitRegs;

  InstructionRequest MainInstr{Opcode, {}};
  const auto &InstrDesc = State.getInstrInfo().get(Opcode);
  // We need to select all operands-registers to insert their
  // initialization according to the valuegram before the main instruction.
  MainInstr.Preselected =
      selectInitializableOperandsRegisters(InstrGenCtx, InstrDesc);

  auto OpsRegs =
      llvm::make_filter_range(MainInstr.Preselected, [](const auto &Operand) {
        return Operand.isReg();
      });
  SmallVector<PreselectedOpInfo> Registers;
  // Added only unique registers
  llvm::copy_if(OpsRegs, std::back_inserter(Registers),
                [Registers](const auto &Operand) {
                  if (llvm::is_contained(Registers, Operand))
                    return false;
                  return true;
                });
  auto RP = InstrGenCtx.pushRegPool();
  llvm::for_each(Registers, [&](const auto &OpReg) {
    auto Reg = OpReg.getReg();
    if (Reg == MCRegister::NoRegister)
      return;
    // To avoid using registers that have already been initialized during
    // initialization.
    llvm::for_each(Tgt.getPhysRegsFromUnit(Reg, RI), [&RP](auto SimpleReg) {
      RP->addReserved(SimpleReg, AccessMaskBit::W);
    });
    // Added initialization instructions
    llvm::append_range(InstrWithInitRegs,
                       generateRegInit(InstrGenCtx, Reg, InstrDesc));
  });

  InstrWithInitRegs.emplace_back(std::move(MainInstr));
  return InstrWithInitRegs;
}

void ValuegramGenPolicy::initialize(InstructionGenerationContext &InstGenCtx,
                                    const RequestLimit &Limit) {
  assert(Limit.isNumLimit());
  assert(Instructions.empty() && Idx == 0 && "Is expected to be called once");

  int PrimaryInstrsLeft = Limit.getLimit() + 1;
  while (--PrimaryInstrsLeft > 0)
    llvm::append_range(Instructions, generateOneInstrWithInitRegs(
                                         InstGenCtx, OpcGen->generate()));
}

APInt ValuegramGenPolicy::getValueFromValuegram(
    Register Reg, StringRef Prefix, InstructionGenerationContext &IGC) const {
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  auto &Cfg = IGC.GenSettings.Cfg;
  assert(Cfg.RegsHistograms);
  const auto &RegsHistograms = Cfg.RegsHistograms.value();
  const auto &ClassHistograms = RegsHistograms.Histograms.ClassHistograms;
  auto It = std::find_if(ClassHistograms.begin(), ClassHistograms.end(),
                         [Prefix](const RegisterClassHistogram &CH) {
                           return CH.RegType == Prefix;
                         });
  auto MCReg = Reg.asMCReg();
  auto BitWidth = Tgt.getRegBitWidth(MCReg, IGC);
  // This means that the histogram contains the required type of registers.
  // We generate the value from the "histograms".
  if (It != ClassHistograms.end()) {
    const auto &Hist = *It;
    const auto &Valuegram = Hist.TheValuegram;
    std::discrete_distribution<size_t> Dist(Valuegram.weights_begin(),
                                            Valuegram.weights_end());
    return sampleValuegramForOneReg(Valuegram, Prefix, BitWidth, Dist);
  }
  // Otherwise, we generate the value from the "registers".
  auto NumReg = Tgt.regToIndex(Reg);
  auto &Fn = IGC.MBB.getParent()->getFunction();
  auto NumRegs =
      Tgt.getNumRegs(Tgt.regToStorage(Reg), State.getSubtargetImpl(Fn));
  std::vector<APInt> APInts(NumRegs);
  getFixedRegisterValues(RegsHistograms, NumRegs, Prefix, BitWidth, APInts);
  if (APInts.size() <= NumReg)
    snippy::fatal(State.getCtx(), "Valuegram error",
                  "No values for " + Prefix + std::to_string(NumReg) +
                      " registers");
  return APInts[NumReg];
}

static std::string getRegistersPrefix(RegStorageType Storage) {
  switch (Storage) {
  case RegStorageType::XReg:
    return "X";
  case RegStorageType::FReg:
    return "F";
  case RegStorageType::VReg:
    return "V";
  }
  llvm_unreachable("Unknown storage type");
}

std::vector<InstructionRequest>
ValuegramGenPolicy::generateRegInit(InstructionGenerationContext &InstrGenCtx,
                                    Register Reg,
                                    const MCInstrDesc &InstrDesc) {
  if (Reg == MCRegister::NoRegister)
    return {};
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  std::vector<InstructionRequest> InitInstrs;
  // We are operating a register group and must write a value
  // to all simple registers in the group.
  llvm::for_each(Tgt.getPhysRegsWithoutOverlaps(Reg, RI), [&](auto SimpleReg) {
    auto ValueToWrite = getValueFromValuegram(
        SimpleReg, getRegistersPrefix(Tgt.regToStorage(SimpleReg)),
        InstrGenCtx);
    SmallVector<MCInst> InstrsForWrite;
    Tgt.generateWriteValueSeq(InstrGenCtx, ValueToWrite, SimpleReg.asMCReg(),
                              InstrsForWrite);
    llvm::transform(
        InstrsForWrite, std::back_inserter(InitInstrs), [&](const auto &I) {
          return InstructionRequest{I.getOpcode(), getPreselectedForInstr(I)};
        });
  });
  return InitInstrs;
}

BurstGenPolicy::BurstGenPolicy(SnippyProgramContext &ProgCtx,
                               const GeneratorSettings &GenSettings,
                               unsigned BurstGroupID) {
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &BGram = GenSettings.getBurstGram();

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
                &OpcCache = ProgCtx.getOpcodeCache()](unsigned Opcode) {
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
  const auto &Cfg = GenSettings.Cfg;
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
  auto &State = InstrGenCtx.ProgCtx.getLLVMState();
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
  auto OpcodeIdxToBaseReg = generateBaseRegs(InstrGenCtx, MemUsers);

  auto RP = InstrGenCtx.pushRegPool();
  auto OpcodeIdxToAI =
      mapOpcodeIdxToAI(InstrGenCtx, OpcodeIdxToBaseReg, MemUsers);
  unsigned MemUsersIdx = 0;
  for (auto &&Instr : Instructions) {
    const auto &InstrDesc = InstrInfo.get(Instr.Opcode);
    if (IsMemUser(Instr.Opcode)) {
      auto BaseReg = OpcodeIdxToBaseReg[MemUsersIdx];
      auto AI = OpcodeIdxToAI[MemUsersIdx];
      auto Preselected = selectOperands(InstrDesc, BaseReg, AI);
      Instr.Preselected =
          selectConcreteOffsets(InstrGenCtx, InstrDesc, Preselected);
      AddressInfo ActualAI = AI;
      auto Offset = getOffsetImmediate(Instr.Preselected);
      ActualAI.Address += Offset.value_or(0);
      markMemAccessAsUsed(InstrGenCtx, InstrDesc, ActualAI,
                          MemAccessKind::BURST, InstrGenCtx.MAI);
      ++MemUsersIdx;
    }
  }
}

LLVMState &InstructionGenerationContext::getLLVMStateImpl() const {
  return ProgCtx.getLLVMState();
}

GenPolicy
createGenPolicy(SnippyProgramContext &ProgCtx,
                const GeneratorSettings &GenSettings,
                const MachineBasicBlock &MBB,
                std::unordered_map<unsigned, double> WeightOverrides) {
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto Filter = Tgt.getDefaultPolicyFilter(ProgCtx, MBB);
  auto MustHavePrimaryInstrs = Tgt.groupMustHavePrimaryInstr(ProgCtx, MBB);
  auto Overrides = Tgt.getPolicyOverrides(ProgCtx, MBB);
  if (GenSettings.isApplyValuegramEachInstr())
    return planning::ValuegramGenPolicy(ProgCtx, GenSettings, std::move(Filter),
                                        MustHavePrimaryInstrs,
                                        std::move(Overrides), WeightOverrides);
  return planning::DefaultGenPolicy(ProgCtx, GenSettings, std::move(Filter),
                                    MustHavePrimaryInstrs, std::move(Overrides),
                                    WeightOverrides);
}

} // namespace planning
} // namespace snippy
} // namespace llvm
