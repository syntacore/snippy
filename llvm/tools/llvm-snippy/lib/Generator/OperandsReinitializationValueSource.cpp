//===-- OperandsReinitializationValueSource.cpp -----------------*- C++ -*-===//
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

// FIXME: Layer violation. This code should be target-independent.
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

Expected<std::optional<APInt>>
OperandsReinitializationValuegramSource::sampleRegisterOperandValueForInstr(
    MCOperand Operand, unsigned OperandIndex,
    [[maybe_unused]] const MCInstrDesc &InstrDesc,
    InstructionGenerationContext &IGC) {
  const auto &RegsHistograms = getRegHistograms();
  const auto &ClassHistograms = RegsHistograms.Histograms.ClassHistograms;
  using ReturnType = Expected<std::optional<APInt>>;

  // Can only handle register operands. 'Fixing/freezing' immediate values
  // can be a valid use-case for some operand sources. Like direct selection
  // of floating-point rounding modes.
  if (!Operand.isReg())
    // Being explicit about return type because CTAD is broken for some older
    // toolchains.
    return ReturnType(std::nullopt);

  const auto &ProgCtx = IGC.ProgCtx;
  const auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  Register Reg = Operand.getReg();
  auto Prefix = getRegistersPrefix(Tgt.regToStorage(Reg));

  // FIXME: Layer violation. This code doesn't have to know about arch-specific
  // register prefix names.
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
  auto NumRegs = Tgt.getNumRegs(Tgt.regToStorage(Reg), IGC.getSubtargetImpl());
  ScratchAPInts.resize(NumRegs);
  getFixedRegisterValues(RegsHistograms, NumRegs, Prefix, BitWidth,
                         ScratchAPInts);
  if (ScratchAPInts.size() <= NumReg)
    return makeFailure(Errc::InvalidConfiguration, "Valuegram error",
                       "No values for " + Prefix + std::to_string(NumReg) +
                           " registers");

  return ScratchAPInts[NumReg];
}

Expected<std::optional<APInt>> OperandsReinitializationOpcodeValuegramSource::
    sampleRegisterOperandValueForInstr(MCOperand Operand, unsigned OperandIndex,
                                       const MCInstrDesc &InstrDesc,
                                       InstructionGenerationContext &IGC) {
  const auto &OpcodeMap = getOpcodeToValuegramMap();
  auto Opcode = InstrDesc.getOpcode();
  using ReturnType = Expected<std::optional<APInt>>;

  if (!Operand.isReg())
    return ReturnType(std::nullopt);

  const auto &ProgCtx = IGC.ProgCtx;
  const auto &OpCC = ProgCtx.getOpcodeCache();
  const auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  Register Reg = Operand.getReg();
  auto MCReg = Reg.asMCReg();
  auto BitWidth = Tgt.getRegBitWidth(MCReg, IGC);
  const auto &OpcodeValuegram = OpcodeMap.getConfigForOpcode(Opcode, OpCC);
  std::discrete_distribution<size_t> Dist(OpcodeValuegram.weightsBegin(),
                                          OpcodeValuegram.weightsEnd());

  return sampleOpcodeValuegramForOneReg(OpcodeValuegram, OperandIndex, BitWidth,
                                        Dist, Opcode, State.getInstrInfo());
}

} // namespace planning
} // namespace snippy
} // namespace llvm
