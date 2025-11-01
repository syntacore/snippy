//===-- LoopCanonicalization.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/CodeGen/MachineLoopInfo.h"

namespace llvm {
namespace snippy {
class LLVMState;
struct SimulatorContext;
class ConsecutiveLoopInfo;
struct Branchegram;

bool canonicalizeLoops(MachineFunction &MF, SimulatorContext &SimCtx,
                       MachineLoopInfo &MLI, ConsecutiveLoopInfo &CLI,
                       LLVMState &State, const Branchegram &Branches);

} // namespace snippy
} // namespace llvm
