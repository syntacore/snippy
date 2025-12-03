//===-- OperandsReinitializationValueSource.h -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/Policy.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace snippy {
namespace planning {

/// \brief Interface class for register value sources.
///
/// Serves as a simple mapping {InstrInfo, Operand} -> Value.
/// Behavior should be solely determined by the SnippyTarget
/// and MCInstrDesc.
///
/// All configurable behavior shall be implemented in implementation
/// constructors.
class IOperandsReinitializationValueSource {
public:
  /// \brief Produce a value to initialize register operand with.
  /// \return Optional preselected value or Error, std::nullopt is not an Error
  /// @todo Make IGC const &. Please don't abuse this and don't use this for
  /// arbitrary side effects.
  virtual Expected<std::optional<APInt>>
  sampleRegisterOperandValueForInstr(MCOperand Operand, unsigned OperandIndex,
                                     const MCInstrDesc &InstrDesc,
                                     InstructionGenerationContext &IGC) = 0;

  /// \brief Check if the source has been exhausted.
  ///
  /// Not used correctly but it's a useful abstraction to have in the
  /// interface.
  virtual bool isDone() const { return false; }

  /// \brief Clone this abstract value.
  virtual std::unique_ptr<IOperandsReinitializationValueSource>
  clone() const = 0;

  virtual ~IOperandsReinitializationValueSource() {}
};

/// \brief Simple operand value source that takes values verbatim from the
/// Valuegram.
class OperandsReinitializationValuegramSource
    : public IOperandsReinitializationValueSource {
  const RegistersWithHistograms &RegHistograms;
  /// Scratch vector to avoid reallocating in for each
  /// sampleRegisterOperandValueForInstr.
  std::vector<APInt> ScratchAPInts;

public:
  OperandsReinitializationValuegramSource(
      const RegistersWithHistograms &RegHist)
      : RegHistograms(RegHist) {}

  Expected<std::optional<APInt>> sampleRegisterOperandValueForInstr(
      MCOperand Operand, unsigned OperandIndex, const MCInstrDesc &InstrDesc,
      InstructionGenerationContext &IGC) override;

  const RegistersWithHistograms &getRegHistograms() const & {
    return RegHistograms;
  }

  virtual std::unique_ptr<IOperandsReinitializationValueSource>
  clone() const override {
    return std::make_unique<OperandsReinitializationValuegramSource>(*this);
  }
};

class OperandsReinitializationOpcodeValuegramSource final
    : public IOperandsReinitializationValueSource {
  OperandsReinitializationSampler Sampler;

public:
  OperandsReinitializationOpcodeValuegramSource(
      const WeightedOpcToSettingsMaps &OpcToSettMap)
      : Sampler(OpcToSettMap) {}

  Expected<std::optional<APInt>> sampleRegisterOperandValueForInstr(
      MCOperand Operand, unsigned OperandIndex, const MCInstrDesc &InstrDesc,
      InstructionGenerationContext &IGC) override;

  std::unique_ptr<IOperandsReinitializationValueSource> clone() const override {
    return std::make_unique<OperandsReinitializationOpcodeValuegramSource>(
        *this);
  }
};

} // namespace planning
} // namespace snippy
} // namespace llvm
