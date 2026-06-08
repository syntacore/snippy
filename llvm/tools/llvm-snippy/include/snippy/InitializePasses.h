//===-- InitializePasses.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_TOOLS_LLVM_SNIPPY_INITIALIZEPASSES_H
#define LLVM_TOOLS_LLVM_SNIPPY_INITIALIZEPASSES_H

namespace llvm {
class PassRegistry;
namespace snippy {
void initializeSnippyPasses(PassRegistry &Registry);
} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_INITIALIZEPASSES_H
