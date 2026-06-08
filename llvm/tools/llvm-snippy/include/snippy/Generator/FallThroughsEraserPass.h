//===-- FallThroughsEraserPass.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_FALLTHROUGHSERASERPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_FALLTHROUGHSERASERPASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
namespace llvm {
namespace snippy {

class FallThroughEraserPass final : public MachineFunctionPass {

public:
  static char ID;

  FallThroughEraserPass() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Fall Through Eraser Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_FALLTHROUGHSERASERPASS_H
