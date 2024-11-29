//===-- CFPermutationPass.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/ActiveImmutablePass.h"
#include "snippy/Generator/CFPermutation.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
namespace snippy {

class CFPermutation
    : public ActiveImmutablePass<MachineFunctionPass, ConsecutiveLoopInfo> {
public:
  static char ID;

  CFPermutation()
      : ActiveImmutablePass<MachineFunctionPass, ConsecutiveLoopInfo>(ID) {}

  StringRef getPassName() const override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

} // namespace snippy
} // namespace llvm
