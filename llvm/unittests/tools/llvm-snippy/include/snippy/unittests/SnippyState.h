//===-- SnippyState.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the class-initializer of llvm state.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_SNIPPYSTATE_H
#define LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_SNIPPYSTATE_H

#include "snippy/GeneratorUtils/LLVMState.h"

#include "gtest/gtest.h"

#include <string>
#include <utility>

namespace llvm::snippy::testing {

inline LLVMState getLLVMState(const SelectedTargetInfo &TargetInfo) {
  auto State = LLVMState::create(TargetInfo);
  if (!State)
    snippy::fatal(State.takeError());
  return std::move(*State);
}

inline LLVMState getLLVMState(std::string Triple, std::string MArch,
                              std::string CPU, std::string Features) {
  return getLLVMState(SelectedTargetInfo{std::move(Triple), std::move(MArch),
                                         std::move(CPU), std::move(Features)});
}

struct SnippyState : public ::testing::TestWithParam<SelectedTargetInfo> {
  LLVMState State;

  // for TEST_P
  SnippyState() : State(getLLVMState(GetParam())) {}

  // for TEST_F
  SnippyState(std::string Triple, std::string MArch, std::string CPU,
              std::string Features)
      : State(getLLVMState(std::move(Triple), std::move(MArch), std::move(CPU),
                           std::move(Features))) {}
};
} // namespace llvm::snippy::testing

#endif // LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_SNIPPYSTATE_H
