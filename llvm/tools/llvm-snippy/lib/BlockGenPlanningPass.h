//===-- BlockGenPlanningPass.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Pass.h"

#include "snippy/Generator/BlockGenPlan.h"

namespace llvm {
namespace snippy {

class BlockGenPlanning final : public MachineFunctionPass {
  BlocksGenPlanTy Plan;

public:
  static char ID;

  BlockGenPlanning();

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  const BlocksGenPlanTy &get() const { return Plan; }
  const SingleBlockGenPlanTy &get(const MachineBasicBlock &MBB) const;

  bool runOnMachineFunction(MachineFunction &MF) override;
  void releaseMemory() override;
};

} // namespace snippy
} // namespace llvm
