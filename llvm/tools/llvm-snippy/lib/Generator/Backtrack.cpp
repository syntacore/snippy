//===-- Backtrack.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Backtrack.h"

#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Target/Target.h"

#define DEBUG_TYPE "snippy-backtrack"
namespace llvm {
namespace snippy {

// namespace for validation functions
namespace {
bool processDiv(const MachineInstr &MI, const Interpreter &I) {
  const auto &DividerOp = getDividerOp(MI);
  auto DividerRegVal = I.readReg(DividerOp.getReg());

  if (DividerRegVal == 0) {
    LLVM_DEBUG(dbgs() << "Division by zero in generated instruction\n";);
    return false;
  }
  return true;
}
} // anonymous namespace

bool Backtrack::isInstrValid(const MachineInstr &MI,
                             const SnippyTarget &ST) const {
  if (ST.isDivOpcode(MI.getOpcode()))
    return processDiv(MI, I);

  return true;
}
} // namespace snippy
} // namespace llvm
