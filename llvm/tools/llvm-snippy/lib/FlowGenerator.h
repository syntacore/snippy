//===-- FlowGenerator.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Classes that handle flow generation.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Config.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/SnippyModule.h"

namespace llvm {
namespace snippy {

class OpcodeCache;
class LLVMState;

struct DebugOptions {
  bool PrintInstrs;
  bool PrintMachineFunctions;
  bool PrintControlFlowGraph;
  bool ViewControlFlowGraph;
};

class FlowGenerator {
  const OpcodeCache &OpCC;
  Config Cfg;
  RegPool RP;
  std::string BaseFileName;

public:
  FlowGenerator(Config &&Cfg, const OpcodeCache &OpCache, RegPool Pool,
                StringRef BaseFileName)
      : OpCC(OpCache), Cfg(std::move(Cfg)), RP(std::move(Pool)),
        BaseFileName(BaseFileName) {}

  GeneratorResult generate(LLVMState &State, const DebugOptions &DebugCfg);
};

} // namespace snippy
} // namespace llvm
