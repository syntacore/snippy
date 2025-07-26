//===-- TrackLivenessPass.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
namespace snippy {

class TrackLiveness : public MachineFunctionPass {

public:
  static char ID;

  TrackLiveness();

  StringRef getPassName() const override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void computeAndAddLiveInsForMBB(MachineBasicBlock &MBB,
                                  const TargetRegisterInfo &TRI);
};

} // namespace snippy
} // namespace llvm
