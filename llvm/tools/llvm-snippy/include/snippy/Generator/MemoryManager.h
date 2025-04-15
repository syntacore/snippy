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

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
namespace snippy {
namespace planning {

class InstructionGenerationContext;

}
class Linker;
class GeneratorContext;
class Interpreter;
class LLVMState;
class SnippyProgramContext;

struct GlobalCodeFlowInfo;

} // namespace snippy
} // namespace llvm
