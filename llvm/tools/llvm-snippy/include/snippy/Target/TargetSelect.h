//===-- TargetSelect.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Utilities to handle the creation of the native snippy target.
///
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {
namespace snippy {

// forward-declarations to minimize includes
void InitializeRISCVSnippyTarget();
void InitializeX86SnippyTarget();

// Initializes all existing snippy targets
inline void InitializeAllSnippyTargets() {
#define SNIPPY_TARGET(TargetName) Initialize##TargetName##SnippyTarget();
#include "llvm/Config/SnippyTargets.def"
}

} // namespace snippy
} // namespace llvm
