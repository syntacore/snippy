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

#include "snippy/Generator/GenerationRequest.h"

#include <optional>

namespace llvm {
namespace snippy {

class BlockGenPlanning final : public MachineFunctionPass {
  std::optional<planning::FunctionRequest> Req;

public:
  static char ID;

  BlockGenPlanning();

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  planning::FunctionRequest &get() {
    assert(Req.has_value());
    return *Req;
  }
  const planning::BasicBlockRequest &get(const MachineBasicBlock &MBB) const;

  bool runOnMachineFunction(MachineFunction &MF) override;
  void releaseMemory() override;
};

} // namespace snippy
} // namespace llvm
