//===--- RISCVInstructionEnumerator.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_LIB_RISCV_RISCV_INSTRUCTION_ENUMERATOR_H
#define LLVM_TOOLS_LLVM_IE_LIB_RISCV_RISCV_INSTRUCTION_ENUMERATOR_H

#include "InstructionEnumerator.h"
#include "TargetEnum.h"

namespace llvm_ie {

class RISCVInstructionEnumerator : public InstructionEnumerator {
public:
  RISCVInstructionEnumerator() = default;

  static void initialize();

  static void terminate();

  std::set<llvm::StringRef> enumerateInstructions() const override;

  llvm::Expected<std::vector<llvm::StringRef> &>
  obtainInstructionCategories(llvm::StringRef Instr) const override;

  static InstructionEnumerator::PluginID getPluginID() {
    return TargetEnum::eTargetRISCV;
  }

  static InstructionEnumerator::PluginCreateCallback getPluginCreateCallback() {
    return []() -> std::unique_ptr<InstructionEnumerator> {
      return std::make_unique<RISCVInstructionEnumerator>();
    };
  }
};

} // namespace llvm_ie

#endif
