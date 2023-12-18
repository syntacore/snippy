//===-- Backtrack.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_BACKTRACK_H
#define LLVM_TOOLS_SNIPPY_BACKTRACK_H

#include "snippy/Generator/Interpreter.h"

#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineModuleInfo.h"

#include <memory>

namespace llvm {
namespace snippy {

class Backtrack {
  Interpreter &I;

public:
  Backtrack(Interpreter &I) : I(I) {}

  bool isInstrValid(const MachineInstr &MI, const SnippyTarget &ST) const;
};

} // namespace snippy
} // namespace llvm

#endif
