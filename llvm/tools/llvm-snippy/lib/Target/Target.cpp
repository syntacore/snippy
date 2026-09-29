//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Target/Target.h"

namespace llvm {
namespace snippy {

SnippyTarget::~SnippyTarget() {} // anchor.

static SmallVectorImpl<const SnippyTarget *> &getRegisteredTargets() {
  // Not using global static to avoid initialization before main.
  static SmallVector<const SnippyTarget *> Targets;
  return Targets;
}

const SnippyTarget *SnippyTarget::lookup(Triple TT) {
  auto &Targets = getRegisteredTargets();
  auto It = find_if(Targets, [&TT](const SnippyTarget *T) {
    return T->matchesArch(TT.getArch());
  });
  if (It == Targets.end())
    return nullptr;
  return *It;
}

void SnippyTarget::registerTarget(SnippyTarget *Target) {
  auto &Targets = getRegisteredTargets();
  if (!is_contained(Targets, Target))
    Targets.push_back(Target);
}

} // namespace snippy
} // namespace llvm
