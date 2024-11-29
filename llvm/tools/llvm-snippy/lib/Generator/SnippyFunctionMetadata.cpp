//===-- SnippyFunctionMetadata.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SnippyFunctionMetadata.h"
#include "../InitializePasses.h"
#include "snippy/CreatePasses.h"

#define DEBUG_TYPE "snippy-function-metadata"
#define PASS_DESC "Snippy Function Metadata"

namespace llvm {
ImmutablePass *createSnippyFunctionMetadataWrapperPass() {
  return new snippy::SnippyFunctionMetadataWrapper();
}
namespace snippy {

char SnippyFunctionMetadataWrapper::ID = 0;

}
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SnippyFunctionMetadataWrapper;

INITIALIZE_PASS(SnippyFunctionMetadataWrapper, DEBUG_TYPE, PASS_DESC, false,
                true)
