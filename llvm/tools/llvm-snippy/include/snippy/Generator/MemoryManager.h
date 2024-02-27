//===-- MemoryManager.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/LLVMState.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
namespace snippy {

class Linker;
class GeneratorContext;
class Interpreter;

struct MemorySectionConfig {
  MemAddr Start = 0;
  MemAddr Size = 0;
  std::string Name;
  MemorySectionConfig() = default;
  MemorySectionConfig(MemAddr Start, MemAddr Size, StringRef Name)
      : Start{Start}, Size{Size}, Name{Name} {};
};

struct MemoryConfig {
  std::vector<MemorySectionConfig> ProgSections;
  MemorySectionConfig Rom;
  MemorySectionConfig Ram;

  static MemoryConfig getMemoryConfig(const Linker &L);
};

} // namespace snippy
} // namespace llvm
