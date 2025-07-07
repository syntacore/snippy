//===-- OperandsReinitializationPolicy.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/OperandsReinitialiazationPolicy.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Target/Target.h"

namespace llvm {
namespace snippy {

namespace planning {

std::vector<planning::PreselectedOpInfo>
getPreselectedForInstr(const MCInst &Inst) {
  using planning::PreselectedOpInfo;
  std::vector<PreselectedOpInfo> Preselected;
  Preselected.reserve(Inst.getNumOperands());
  transform(Inst, std::back_inserter(Preselected), [](const auto &Operand) {
    auto OpOrErr = PreselectedOpInfo::fromMCOperand(Operand);
    return unwrapOrFatal(std::move(OpOrErr));
  });
  return Preselected;
}

static bool isDestinationRegister(unsigned OpIndex, unsigned NumDefs) {
  // NumDefs - number of MachineOperands that are register definitions.
  // Register definitions always occur at the start of the machine operand list.
  return OpIndex < NumDefs;
}

static Register pregenerateRegister(InstructionGenerationContext &InstrGenCtx,
                                    const MCInstrDesc &InstrDesc,
                                    const MCOperandInfo &MCOpInfo,
                                    unsigned OpIndex) {
  auto &MBB = InstrGenCtx.MBB;
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &State = ProgCtx.getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &Tgt = State.getSnippyTarget();
  auto OperandRegClassID = InstrDesc.operands()[OpIndex].RegClass;
  auto RegClass = Tgt.getRegClass(InstrGenCtx, OperandRegClassID, OpIndex,
                                  InstrDesc.getOpcode(), RegInfo);
  auto Exclude =
      Tgt.excludeRegsForOperand(InstrGenCtx, RegClass, InstrDesc, OpIndex);
  auto Include = Tgt.includeRegs(InstrDesc.getOpcode(), RegClass);
  AccessMaskBit Mask = AccessMaskBit::RW;
  auto CustomMask = Tgt.getCustomAccessMaskForOperand(InstrDesc, OpIndex);
  if (CustomMask != AccessMaskBit::None)
    Mask = CustomMask;
  auto RegOpt =
      ProgCtx.getRegGen().generate(RegClass, OperandRegClassID, RegInfo, RP,
                                   MBB, Tgt, Exclude, Include, Mask);
  assert(RegOpt.has_value());
  return RegOpt.value();
}

/// Pregenarate available register operands.
/// \return Vector of size InstrDesc.getNumOperands(). Uninitializable operands
/// have corresponding PreselectedOpInfo::isUnset().
std::vector<planning::PreselectedOpInfo>
selectInitializableOperandsRegisters(InstructionGenerationContext &InstrGenCtx,
                                     const MCInstrDesc &InstrDesc) {
  std::vector<planning::PreselectedOpInfo> Preselected;
  Preselected.reserve(InstrDesc.getNumOperands());
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  bool ValuegramOperandsRegsInitOutputs =
      InstrGenCtx.hasCfg<DefaultPolicyConfig>() &&
      InstrGenCtx.getCfg<DefaultPolicyConfig>().Valuegram &&
      InstrGenCtx.getCfg<DefaultPolicyConfig>()
          .Valuegram->ValuegramOperandsRegsInitOutputs;
  // If the register is a destination register and we don't want to initialize
  // the outputs, we skip it.
  auto NeedsInit = [&](auto OpIndex) {
    return !isDestinationRegister(OpIndex, InstrDesc.getNumDefs()) ||
           ValuegramOperandsRegsInitOutputs;
  };
  llvm::transform(
      llvm::enumerate(InstrDesc.operands()), std::back_inserter(Preselected),
      [&, &Tgt = ProgCtx.getLLVMState().getSnippyTarget()](
          const auto &&Args) -> planning::PreselectedOpInfo {
        const auto &[OpIndex, MCOpInfo] = Args;
        // If it is TIED_TO, this register is already
        // selected.
        if (NeedsInit(OpIndex) &&
            Tgt.canInitializeOperand(InstrDesc, OpIndex) &&
            InstrDesc.getOperandConstraint(OpIndex, MCOI::TIED_TO) == -1)
          return pregenerateRegister(InstrGenCtx, InstrDesc, MCOpInfo, OpIndex);
        return {};
      });
  return Preselected;
}

ValuegramGenPolicy::ValuegramGenPolicy(
    SnippyProgramContext &ProgCtx, const DefaultPolicyConfig &Cfg,
    std::function<bool(unsigned)> Filter, bool MustHavePrimaryInstrs,
    ArrayRef<OpcodeHistogramEntry> Overrides,
    const std::unordered_map<unsigned, double> &WeightOverrides,
    std::unique_ptr<IOperandsReinitializationValueSource> ValueSource)
    : OpcGen(Cfg.createOpcodeGenerator(
          /*OpCC=*/ProgCtx.getOpcodeCache(), /*OpcMask=*/Filter,
          /*Overrides=*/Overrides,
          /*MustHavePrimaryInstrs=*/MustHavePrimaryInstrs,
          /*OpcWeightOverrides=*/WeightOverrides)),
      Cfg(&Cfg), OperandsValueSource(std::move(ValueSource)) {
  assert(Cfg.isApplyValuegramEachInstr() &&
         "This policy can only be used when the "
         "-valuegram-operands-regs file provided");
  assert(OperandsValueSource);
}

std::vector<InstructionRequest>
ValuegramGenPolicy::generateOneInstrWithInitRegs(
    InstructionGenerationContext &InstrGenCtx, unsigned Opcode) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  std::vector<InstructionRequest> InstrWithInitRegs;

  // We need to select all operands-registers to insert their
  // initialization according to the valuegram before the main instruction.
  const auto &InstrDesc = State.getInstrInfo().get(Opcode);
  InstructionRequest MainInstr{
      Opcode, selectInitializableOperandsRegisters(InstrGenCtx, InstrDesc)};

  // TODO: Support selecting immediate operand values. This can be handled by
  // setting Min = Max in the StridedImmediate.
  auto OpsRegs =
      llvm::make_filter_range(MainInstr.Preselected, [](const auto &Operand) {
        return Operand.isReg();
      });

  SmallVector<PreselectedOpInfo> Registers;
  // Simple quadratic unique because PreselectedOpInfo/StridedImmediate
  // don't have an order defined, so std::unique is not applicable.
  for (auto Operand : OpsRegs) {
    if (!llvm::is_contained(Registers, Operand))
      Registers.push_back(Operand);
  }

  auto RP = InstrGenCtx.pushRegPool();
  llvm::for_each(llvm::enumerate(Registers), [&](const auto &OpWithIndex) {
    auto &&[OperandIndex, Op] = OpWithIndex;
    // Skip operands that can't be initialized.
    if (Op.isUnset())
      return;

    assert(Op.isReg()); // TODO: Support immediate operands and handle TiedTo
    auto Reg = Op.getReg();
    assert(Reg != MCRegister::NoRegister); // Should not happen

    // To avoid using registers that have already been initialized during
    // initialization.
    llvm::for_each(Tgt.getPhysRegsFromUnit(Reg, RI), [&RP](auto SimpleReg) {
      RP->addReserved(SimpleReg, AccessMaskBit::W);
    });
    // Added initialization instructions
    llvm::append_range(
        InstrWithInitRegs,
        generateRegInit(InstrGenCtx, OperandIndex, Reg, InstrDesc));
  });

  InstrWithInitRegs.emplace_back(std::move(MainInstr));
  return InstrWithInitRegs;
}

void ValuegramGenPolicy::initialize(InstructionGenerationContext &InstGenCtx,
                                    const RequestLimit &Limit) {
  InstGenCtx.switchConfig(*Cfg);
  assert(Limit.isNumLimit());
  assert(Instructions.empty() && Idx == 0 && "Is expected to be called once");

  int PrimaryInstrsLeft = Limit.getLimit() + 1;
  while (--PrimaryInstrsLeft > 0)
    llvm::append_range(Instructions, generateOneInstrWithInitRegs(
                                         InstGenCtx, OpcGen->generate()));
}

Expected<std::optional<APInt>> ValuegramGenPolicy::getValueFromValuegram(
    MCOperand Op, unsigned OperandIndex, const MCInstrDesc &InstrDesc,
    InstructionGenerationContext &IGC) const {
  assert(OperandsValueSource);
  return OperandsValueSource->sampleRegisterOperandValueForInstr(
      Op, OperandIndex, InstrDesc, IGC);
}

std::vector<InstructionRequest>
ValuegramGenPolicy::generateRegInit(InstructionGenerationContext &InstrGenCtx,
                                    unsigned OperandIndex, Register Reg,
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
  llvm::for_each(
      Tgt.getPhysRegsWithoutOverlaps(Reg, RI), [&](Register SimpleReg) {
        auto ValueToWriteOrError =
            getValueFromValuegram(MCOperand::createReg(SimpleReg), OperandIndex,
                                  InstrDesc, InstrGenCtx);

        // TODO: Better error handling.
        if (!ValueToWriteOrError)
          snippy::fatal(ValueToWriteOrError.takeError());

        auto &ValueToWrite = *ValueToWriteOrError;
        if (!ValueToWrite.has_value())
          return;
        SmallVector<MCInst> InstrsForWrite;
        Tgt.generateWriteValueSeq(InstrGenCtx, *ValueToWrite,
                                  SimpleReg.asMCReg(), InstrsForWrite);
        llvm::transform(InstrsForWrite, std::back_inserter(InitInstrs),
                        [&](const auto &I) {
                          return InstructionRequest{I.getOpcode(),
                                                    getPreselectedForInstr(I)};
                        });
      });
  return InitInstrs;
}

} // namespace planning
} // namespace snippy
} // namespace llvm
