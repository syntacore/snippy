//===-- OperandsReinitialiazationPolicy.h -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/OperandsReinitializationValueSource.h"
#include "snippy/Generator/Policy.h"

namespace llvm {
namespace snippy {
namespace planning {

/// \brief This generation policy initializes the registers according to the
/// valuegram-operands-regs option.
///
/// \details That is, it inserts additional initializing instructions before
/// each instruction for registers used in it that are not memory addresses.
/// These initializing instructions are not taken into account in the planning
/// instructions.
class ValuegramGenPolicy final : public detail::EmptyFinalizeMixin {
  OpcGenHolder OpcGen;
  const DefaultPolicyConfig *Cfg;

  /// Abstract source of (register) operand values.
  std::unique_ptr<IOperandsReinitializationValueSource> OperandsValueSource;
  std::vector<InstructionRequest> Instructions;
  unsigned Idx = 0;

public:
  ValuegramGenPolicy(const ValuegramGenPolicy &Other)
      : OpcGen(Other.OpcGen->copy()), Cfg(Other.Cfg),
        OperandsValueSource(Other.OperandsValueSource->clone()) {}

  ValuegramGenPolicy(ValuegramGenPolicy &&) = default;

  ValuegramGenPolicy &operator=(const ValuegramGenPolicy &Other) {
    ValuegramGenPolicy Tmp = Other;
    std::swap(*this, Tmp);
    return *this;
  }

  ValuegramGenPolicy &operator=(ValuegramGenPolicy &&) = default;

  ValuegramGenPolicy(
      SnippyProgramContext &ProgCtx, const DefaultPolicyConfig &Cfg,
      std::function<bool(unsigned)> Filter, bool MustHavePrimaryInstrs,
      ArrayRef<OpcodeHistogramEntry> Overrides,
      const std::unordered_map<unsigned, double> &WeightOverrides,
      std::unique_ptr<IOperandsReinitializationValueSource>
          OperandsValueSource);

  std::optional<InstructionRequest> next() {
    assert(Idx <= Instructions.size());
    if (Idx < Instructions.size())
      return Instructions[Idx++];
    return std::nullopt;
  }

  void initialize(InstructionGenerationContext &InstrGenCtx,
                  const RequestLimit &Limit);

  bool isInseparableBundle() const { return true; }

  void print(raw_ostream &OS) const { OS << "Valuegram Generation Policy\n"; }

private:
  std::vector<InstructionRequest>
  generateRegInit(InstructionGenerationContext &IGC, unsigned OperandIndex,
                  Register Reg, const MCInstrDesc &InstrDesc);

  std::vector<InstructionRequest>
  generateOneInstrWithInitRegs(InstructionGenerationContext &IGC,
                               unsigned Opcode);

  Expected<std::optional<APInt>>
  getValueFromValuegram(MCOperand Op, unsigned OperandIndex,
                        const MCInstrDesc &InstrDesc,
                        InstructionGenerationContext &IGC) const;
};

} // namespace planning
} // namespace snippy
} // namespace llvm
