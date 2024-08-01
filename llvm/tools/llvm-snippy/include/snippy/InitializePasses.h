//===-- InitializePasses.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

namespace llvm {
class PassRegistry;
namespace snippy {
void initializeSnippyPasses(PassRegistry &Registry);
} // namespace snippy
} // namespace llvm
