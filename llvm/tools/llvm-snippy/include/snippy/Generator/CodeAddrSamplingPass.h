//===-- CodeAddrSamplingPass.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLINGPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLINGPASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
namespace snippy {

class PassConfig;

class CodeAddrSampling final : public MachineFunctionPass {

public:
  static char ID;

  CodeAddrSampling() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Snippy Code Addresses Sampling Pass";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLINGPASS_H
