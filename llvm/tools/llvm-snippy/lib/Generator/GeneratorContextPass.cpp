//===-- GeneratorContextPass.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/CreatePasses.h"

#define DEBUG_TYPE "snippy-generator-context"
#define PASS_DESC "Snippy Generator Context"

char llvm::snippy::GeneratorContextWrapper::ID = 0;

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::GeneratorContextWrapper;

INITIALIZE_PASS(GeneratorContextWrapper, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

ImmutablePass *
createGeneratorContextWrapperPass(snippy::GeneratorContext &Context) {
  return new GeneratorContextWrapper(Context);
}

GeneratorContextWrapper::GeneratorContextWrapper(GeneratorContext &Context)
    : ImmutablePass(ID), Context(&Context) {
  initializeGeneratorContextWrapperPass(*PassRegistry::getPassRegistry());
}

} // namespace llvm
