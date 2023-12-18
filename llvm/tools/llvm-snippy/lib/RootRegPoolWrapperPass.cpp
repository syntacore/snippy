//===-- RootRegPoolWrapperPass.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RootRegPoolWrapperPass.h"
#include "CreatePasses.h"
#include "InitializePasses.h"

#define DEBUG_TYPE "snippy-root-regpool-wrapper"
#define PASS_DESC "Snippy Root Register Pool Wrapper"

char llvm::snippy::RootRegPoolWrapper::ID = 0;

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::RootRegPoolWrapper;

INITIALIZE_PASS(RootRegPoolWrapper, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

ImmutablePass *createRootRegPoolWrapperPass() {
  return new RootRegPoolWrapper();
}

} // namespace llvm

namespace llvm {
namespace snippy {

StringRef RootRegPoolWrapper::getPassName() const { return PASS_DESC " Pass"; }

} // namespace snippy
} // namespace llvm
