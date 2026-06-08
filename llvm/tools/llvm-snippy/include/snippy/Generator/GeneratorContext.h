//===-- GeneratorContext.h -------  -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXT_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXT_H

#include "snippy/Config/Config.h"
#include "snippy/Generator/SnippyModule.h"
#include "snippy/Generator/TopMemAccSampler.h"

namespace llvm {
namespace snippy {

class GeneratorContext {
private:
  SnippyProgramContext *ProgContext = nullptr;

  Config *Cfg = nullptr;

  TopLevelMemoryAccessSampler MemAccSampler;

public:
  GeneratorContext(SnippyProgramContext &ProgContext, Config &Cfg);

  const auto &getProgramContext() const { return *ProgContext; }

  auto &getProgramContext() { return *ProgContext; }
  auto &getMemoryAccessSampler() { return MemAccSampler; }

  auto &getConfig() const {
    assert(Cfg);
    return *Cfg;
  }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXT_H
