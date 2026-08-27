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

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "gtest/gtest.h"
#include <string>

namespace llvm::snippy::testing {

struct SnippyState : public ::testing::TestWithParam<SelectedTargetInfo> {
  LLVMState State;

  // for TEST_P
  SnippyState() : State(parseErrorAndGetState(GetParam())) {}

  // for TEST_F
  SnippyState(std::string Triple, std::string MArch, std::string CPU,
              std::string Features)
      : State(parseErrorAndGetState(
            SelectedTargetInfo{Triple, MArch, CPU, Features})) {}

private:
  static LLVMState parseErrorAndGetState(const SelectedTargetInfo &TargetInfo) {
    auto State = LLVMState::create(TargetInfo);
    if (!State)
      snippy::fatal(State.takeError());
    return std::move(*State);
  }
};
} // namespace llvm::snippy::testing

#endif // LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_SNIPPYSTATE_H
