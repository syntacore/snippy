//===-- GeneratorContextPass.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GeneratorContext.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Pass.h"

namespace llvm {
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
