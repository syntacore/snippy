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

struct MemoryConfig {
  MemAddr ProgSectionStart = 0;
  MemAddr ProgSectionSize = 0;
  std::string ProgSectionName;

  MemAddr RomStart = 0;
  MemAddr RomSize = 0;
  std::string RomSectionName; // Empty if ROM section is not present

  MemAddr RamStart = 0;
  MemAddr RamSize = 0;
  static MemoryConfig getMemoryConfig(const Linker &L);
};

} // namespace snippy
} // namespace llvm
