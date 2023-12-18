//===-- RootRegPoolWrapperPass.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "GeneratorContextPass.h"
#include "InitializePasses.h"
#include "snippy/Generator/RegisterPool.h"

#include "llvm/Pass.h"

namespace llvm {
namespace snippy {

class RegPool;

class RootRegPoolWrapper final : public ImmutablePass {
public:
  static char ID;

  RootRegPoolWrapper() : ImmutablePass(ID) {
    initializeRootRegPoolWrapperPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
    ImmutablePass::getAnalysisUsage(AU);
  }

  RegPool &getPool() {
    return getAnalysis<GeneratorContextWrapper>()
        .getContext()
        .RegPoolsStorage.front();
  }

  const RegPool &getPool() const {
    return getAnalysis<GeneratorContextWrapper>()
        .getContext()
        .RegPoolsStorage.front();
  }
};

} // namespace snippy
} // namespace llvm
