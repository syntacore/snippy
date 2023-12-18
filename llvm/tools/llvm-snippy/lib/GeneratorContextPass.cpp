//===-- GeneratorContextPass.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GeneratorContextPass.h"
#include "CreatePasses.h"
#include "InitializePasses.h"

#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

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
