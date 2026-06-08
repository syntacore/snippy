//===-- RootRegPoolWrapperPass.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_ROOTREGPOOLWRAPPERPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_ROOTREGPOOLWRAPPERPASS_H

#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SnippyModule.h"
#include "snippy/GeneratorUtils/RegisterPool.h"

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
    auto &PC =
        getAnalysis<GeneratorContextWrapper>().getContext().getProgramContext();
    auto &State = PC.getLLVMState();
    return RegPoolWrapper(RegPoolWrapper::CreateRoot{}, State.getSnippyTarget(),
                          State.getRegInfo(), PC.RegPoolsStorage);
  }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_ROOTREGPOOLWRAPPERPASS_H
