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
#include "snippy/Support/Options.h"

namespace llvm {
namespace snippy {
#define GEN_SNIPPY_OPTIONS_STRUCT_DEF
#include "SnippyDriverOptionsStruct.inc"
#undef GEN_SNIPPY_OPTIONS_STRUCT_DEF

class OpcodeCache;
class LLVMState;

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
