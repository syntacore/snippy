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

// TODO: remove this dependency
#include "snippy/Generator/GeneratorContext.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
namespace snippy {

class MemoryScheme;
class OpcodeCache;
class LLVMState;
class RegPool;
struct RegisterState;
struct GeneratorSettings;
struct ImmediateHistogram;

class FlowGenerator {
  const OpcodeCache &OpCC;
  GeneratorSettings GenSettings;
  RegPool RP;

public:
  FlowGenerator(GeneratorSettings &&Cfg, const OpcodeCache &OpCache,
                RegPool Pool)
      : OpCC(OpCache), GenSettings(std::move(Cfg)), RP(std::move(Pool)) {}

  GeneratorResult generate(LLVMState &State);
};

} // namespace snippy
} // namespace llvm
