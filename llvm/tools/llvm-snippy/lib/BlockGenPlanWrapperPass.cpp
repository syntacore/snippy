//===-- BlockGenPlanWrapperPass.cpp ------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/BlockGenPlanWrapperPass.h"
#include "InitializePasses.h"
#include "snippy/CreatePasses.h"

#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "snippy-gen-plan-wrapper"
#define PASS_DESC "Snippy generation plan immutable container"

char llvm::snippy::BlockGenPlanWrapper::ID = 0;

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::BlockGenPlanWrapper;

INITIALIZE_PASS(BlockGenPlanWrapper, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

ImmutablePass *createBlockGenPlanWrapperPass() {
  return new BlockGenPlanWrapper();
}

BlockGenPlanWrapper::BlockGenPlanWrapper() : ImmutablePass(ID) {
  initializeBlockGenPlanWrapperPass(*PassRegistry::getPassRegistry());
}

} // namespace llvm
