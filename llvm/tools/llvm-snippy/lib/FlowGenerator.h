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
#include "snippy/Generator/SnippyModule.h"
#include "snippy/GeneratorUtils/RegisterPool.h"
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
  std::unique_ptr<ProgramConfig> ProgramCfg;
  Config Cfg;
  std::vector<RegPool> RegPools;
  std::string BaseFileName;

public:
  FlowGenerator(std::unique_ptr<ProgramConfig> &&ProgCfg, Config &&Cfg,
                const OpcodeCache &OpCache, std::vector<RegPool> Pools,
                StringRef BaseFileName)
      : OpCC(OpCache), ProgramCfg(std::move(ProgCfg)), Cfg(std::move(Cfg)),
        RegPools(std::move(Pools)), BaseFileName(BaseFileName) {}

  GeneratorResult generate(LLVMState &State, const DebugOptions &DebugCfg);
};

} // namespace snippy
} // namespace llvm
