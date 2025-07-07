//===-- RootRegPoolWrapperPass.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/SnippyModule.h"

namespace llvm {

void initializeRootRegPoolWrapperPass(PassRegistry &);

namespace snippy {

class RegPool;

class RootRegPoolWrapper final : public ImmutablePass {
public:
  static char ID;

  RootRegPoolWrapper() : ImmutablePass(ID) {}

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    ImmutablePass::getAnalysisUsage(AU);
  }

  RegPoolWrapper getPool() const {
    return RegPoolWrapper(getAnalysis<GeneratorContextWrapper>()
                              .getContext()
                              .getProgramContext());
  }
};

} // namespace snippy
} // namespace llvm
