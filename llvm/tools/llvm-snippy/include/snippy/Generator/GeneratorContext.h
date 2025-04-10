//===-- GeneratorContext.h -------  -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Config.h"
#include "snippy/Generator/SnippyModule.h"
#include "snippy/Generator/TopMemAccSampler.h"

namespace llvm {
namespace snippy {

class GeneratorContext {
private:
  SnippyProgramContext &ProgContext;

  Config *Cfg = nullptr;

  TopLevelMemoryAccessSampler MemAccSampler;

public:
  GeneratorContext(SnippyProgramContext &ProgContext, Config &Cfg);
  ~GeneratorContext();

  const auto &getProgramContext() const { return ProgContext; }

  auto &getProgramContext() { return ProgContext; }
  auto &getMemoryAccessSampler() { return MemAccSampler; }

  auto &getConfig() const {
    assert(Cfg);
    return *Cfg;
  }
};

} // namespace snippy
} // namespace llvm
