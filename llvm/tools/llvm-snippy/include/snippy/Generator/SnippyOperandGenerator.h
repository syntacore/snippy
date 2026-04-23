//==- SnippyOperandGenerator.h - Generate operands for MachineInstr --------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_SNIPPY_INCLUDE_GENERATOR_OPERAND_GENERATOR_H
#define LLVM_SNIPPY_INCLUDE_GENERATOR_OPERAND_GENERATOR_H

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/Policy.h"
#include <algorithm>

namespace llvm {
namespace snippy {
// TODO: rework it to fit with interface of Generation.cpp
class SnippyOperandGenerator {
protected:
  InstructionGenerationContext &InstrGenCtx;
  const MCInstrDesc &InstrDesc;

  constexpr static size_t MaxOperandsNum = 8;
  // Store generated operands inside class state to share generated operands
  // between generators
  SmallVector<MachineOperand, MaxOperandsNum> Operands;

public:
  SnippyOperandGenerator(InstructionGenerationContext &InstrGenCtx,
                         const MCInstrDesc &InstrDesc)
      : InstrGenCtx(InstrGenCtx), InstrDesc(InstrDesc) {};
  // This method should be generated using snippy-tablegen
  virtual void
  generateOperands(ArrayRef<planning::PreselectedOpInfo> Preselected) = 0;
  const SmallVector<MachineOperand, MaxOperandsNum>
  getGeneratedOperands() const {
    assert(Operands.size() == InstrDesc.getNumOperands());
    return Operands;
  }
  template <int64_t MinValue, int64_t MaxValue>
  int64_t getImmediate(const planning::PreselectedOpInfo &Preselected) const {
    assert(MinValue <= MaxValue);
    // FIXME: remove, when IH will support types wider than int
    constexpr int CroppedMinValue =
        std::clamp<int64_t>(MinValue, std::numeric_limits<int>::min(),
                            std::numeric_limits<int>::max());

    constexpr int CroppedMaxValue = std::clamp<int64_t>(
        MaxValue, CroppedMinValue, std::numeric_limits<int>::max());

    if (Preselected.isImm()) {
      StridedImmediate StridedImm = Preselected.getImm();
      if (StridedImm.isInitialized())
        return genImmInInterval<CroppedMinValue, CroppedMaxValue>(StridedImm);
    }
    const CommonPolicyConfig &Cfg = InstrGenCtx.getCommonCfg();
    const auto &IHV = Cfg.ImmHistogram;
    const ImmediateHistogramSequence *Seq;
    if (IHV.holdsAlternative<ImmediateHistogramRegEx>()) {
      const auto &OpcSettings =
          Cfg.ImmHistMap.getConfigForOpcode(InstrDesc.getOpcode());
      if (OpcSettings.isUniform())
        return RandEngine::genInRangeExclusive<int64_t>(MinValue, MaxValue);
      Seq = &OpcSettings.getSequence();
    } else {
      Seq = &IHV.get<ImmediateHistogramSequence>();
    }

    if (Seq->empty())
      return RandEngine::genInRangeExclusive<int64_t>(MinValue, MaxValue);

    return genImmInInterval<CroppedMinValue, CroppedMaxValue>(
        Seq, StridedImmediate());
  }

  static MachineOperand createRegAsOperand(Register Reg, unsigned Flags,
                                           unsigned SubReg = 0) {
    assert((Flags & 0x1) == 0 && "0x1 is not a RegState flag");
    return MachineOperand::CreateReg(
        Reg, Flags & RegState::Define, Flags & RegState::Implicit,
        Flags & RegState::Kill, Flags & RegState::Dead, Flags & RegState::Undef,
        Flags & RegState::EarlyClobber, SubReg, Flags & RegState::Debug,
        Flags & RegState::InternalRead, Flags & RegState::Renamable);
  }

  virtual ~SnippyOperandGenerator() = default;
};
} // namespace snippy
} // namespace llvm

#endif // LLVM_SNIPPY_INCLUDE_GENERATOR_OPERAND_GENERATOR_H
