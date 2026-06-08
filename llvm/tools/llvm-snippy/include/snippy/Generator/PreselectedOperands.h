//===-- PreselectedOperands.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_PRESELECTEDOPERANDS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_PRESELECTEDOPERANDS_H

#include "snippy/Config/ImmediateHistogram.h"

#include "llvm/ADT/SmallVector.h"

#include <variant>

namespace llvm::snippy::planning {
// Helper class to keep additional information about the operand: register
// number that has been somehow selected before instruction generation,
// immediate operand range and so on.
class PreselectedOpInfo {
  using EmptyTy = std::monostate;
  using RegTy = llvm::Register;
  using ImmTy = StridedImmediate;
  using TiedTy = int;
  std::variant<EmptyTy, RegTy, ImmTy, TiedTy> Value;

  unsigned Flags = 0;

public:
  PreselectedOpInfo(llvm::Register R) : Value(R) {}
  PreselectedOpInfo(StridedImmediate Imm) : Value(Imm) {}
  PreselectedOpInfo() = default;
  template <typename OperandTy>
  static Expected<PreselectedOpInfo> fromOperand(const OperandTy &Op) {
    if (Op.isReg())
      return PreselectedOpInfo(Register(Op.getReg()));
    if (Op.isImm())
      return PreselectedOpInfo(StridedImmediate(Op.getImm()));
    return snippy::makeFailure(
        Errc::Unimplemented,
        "Unknown Operand Type while constructing PreselectedOpInfo");
  }

  bool isReg() const { return std::holds_alternative<RegTy>(Value); }
  bool isImm() const { return std::holds_alternative<ImmTy>(Value); }
  bool isUnset() const { return std::holds_alternative<EmptyTy>(Value); }
  bool isTiedTo() const { return std::holds_alternative<TiedTy>(Value); }

  unsigned getFlags() const { return Flags; }
  StridedImmediate getImm() const {
    assert(isImm());
    return std::get<ImmTy>(Value);
  }
  llvm::Register getReg() const {
    assert(isReg());
    return std::get<RegTy>(Value);
  }
  llvm::Register getTiedTo() const {
    assert(isTiedTo());
    return std::get<TiedTy>(Value);
  }

  void setFlags(unsigned F) { Flags = F; }
  void setTiedTo(int OpIdx) {
    assert(isUnset());
    Value = OpIdx;
  }

  friend constexpr bool operator==(const PreselectedOpInfo &Lhs,
                                   const PreselectedOpInfo &Rhs) {
    return Lhs.Value == Rhs.Value && Lhs.Flags == Rhs.Flags;
  }

  void print(raw_ostream &OS) const {
    if (isReg()) {
      OS << "reg(" << getReg() << ")";
    } else if (isImm()) {
      OS << "imm(";
      getImm().print(OS);
      OS << ")";
    } else if (isTiedTo()) {
      OS << "tied(" << getTiedTo() << ")";
    } else {
      OS << "unset";
    }
  }
};

// Typically instructions have very limited number of operands
using PreselectedOperands = SmallVector<PreselectedOpInfo, 8>;

} // namespace llvm::snippy::planning
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_PRESELECTEDOPERANDS_H
