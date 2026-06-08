//===-- GeneratorContextPass.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXTPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXTPASS_H

#include "snippy/Generator/GeneratorContext.h"
#include "llvm/Pass.h"

namespace llvm {

void initializeGeneratorContextWrapperPass(PassRegistry &);

namespace snippy {

class GeneratorContext;

class GeneratorContextWrapper final : public ImmutablePass {
  GeneratorContext *Context = nullptr;

public:
  static char ID;

  GeneratorContextWrapper() : ImmutablePass(ID) {}
  GeneratorContextWrapper(GeneratorContext &Context);

  StringRef getPassName() const override {
    return "Snippy Generator Context Wrapper Pass";
  }

  GeneratorContext &getContext() { return *Context; }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_GENERATORCONTEXTPASS_H
