//===-- BlockGenPlanWrapperPass.h -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GenerationRequest.h"

namespace llvm {
namespace snippy {

class BlockGenPlanWrapper final : public ImmutablePass {
  DenseMap<const MachineFunction *, planning::FunctionRequest> FuncGenPlan;

public:
  static char ID;

  BlockGenPlanWrapper();
  StringRef getPassName() const override {
    return "Snippy Block Generation Plan Wrapper Pass";
  }

  void setFunctionRequest(const MachineFunction *MF,
                          planning::FunctionRequest Req) {
    assert(!FuncGenPlan.contains(MF));
    FuncGenPlan.insert(std::make_pair(MF, Req));
  }

  planning::FunctionRequest &getFunctionRequest(const MachineFunction *MF) {
    assert(FuncGenPlan.contains(MF));
    return FuncGenPlan.find(MF)->second;
  }
};

} // namespace snippy
} // namespace llvm
