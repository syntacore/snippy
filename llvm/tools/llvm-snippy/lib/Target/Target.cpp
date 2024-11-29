//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Target/Target.h"
#include "snippy/Generator/GeneratorContext.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace snippy {

SnippyTarget::~SnippyTarget() {} // anchor.

static SnippyTarget *FirstTarget = nullptr;

const SnippyTarget *SnippyTarget::lookup(Triple TT) {
  for (const SnippyTarget *T = FirstTarget; T != nullptr; T = T->Next) {
    if (T->matchesArch(TT.getArch()))
      return T;
  }
  return nullptr;
}

void SnippyTarget::registerTarget(SnippyTarget *Target) {
  if (FirstTarget == nullptr) {
    FirstTarget = Target;
    return;
  }
  if (Target->Next != nullptr)
    return; // Already registered.
  Target->Next = FirstTarget;
  FirstTarget = Target;
}

void SnippyTarget::generateSpillToAddr(InstructionGenerationContext &IGC,
                                       MCRegister Reg, MemAddr Addr) const {
  storeRegToAddr(IGC, Addr, Reg,
                 /* store the whole register */ 0);
}
void SnippyTarget::generateReloadFromAddr(InstructionGenerationContext &IGC,
                                          MCRegister Reg, MemAddr Addr) const {
  loadRegFromAddr(IGC, Addr, Reg);
}
} // namespace snippy
} // namespace llvm
