//===-- GeneratorContext.h -------  -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GeneratorSettings.h"
#include "snippy/Generator/TopMemAccSampler.h"

namespace llvm {
namespace snippy {

class GeneratorContext {
private:
  SnippyProgramContext &ProgContext;

  GeneratorSettings *GenSettings = nullptr;

  TopLevelMemoryAccessSampler MemAccSampler;

  void diagnoseSelfcheckSection(size_t MinSize) const;

public:
  GeneratorContext(SnippyProgramContext &ProgContext,
                   GeneratorSettings &GenSettings);
  ~GeneratorContext();

  const auto &getProgramContext() const { return ProgContext; }

  auto &getProgramContext() { return ProgContext; }
  auto &getMemoryAccessSampler() { return MemAccSampler; }

  auto &getConfig() const {
    assert(GenSettings);
    return GenSettings->Cfg;
  }

  const auto &getGenSettings() const {
    assert(GenSettings);
    return *GenSettings;
  }
};

} // namespace snippy
} // namespace llvm
