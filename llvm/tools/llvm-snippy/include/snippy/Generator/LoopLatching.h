//===-- LoopLatching.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Config.h"
#include "snippy/Generator/SnippyLoopInfo.h"
#include "snippy/GeneratorUtils/RegisterPool.h"
#include "llvm/CodeGen/MachineLoopInfo.h"

namespace llvm {
namespace snippy {
class LLVMState;
struct SimulatorContext;
class ConsecutiveLoopInfo;
class ProgramContext;
class SnippyLoopInfo;
class RegPoolWrapper;
class Config;

bool latchLoops(MachineFunction &MF, GeneratorContext &SGCtx,
                SimulatorContext &SimCtx, MachineLoopInfo &MLI,
                ConsecutiveLoopInfo &CLI, LLVMState &State, const Config &Cfg,
                SnippyLoopInfo &SLI, RegPoolWrapper &RootPool);

} // namespace snippy
} // namespace llvm
