//===-- InitSnippyGTestEnv.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains snippy's gtest environment and run all snippy's
/// unit-tests.
///
//===----------------------------------------------------------------------===//

#include "snippy/Target/TargetSelect.h"

#include "snippy/Support/RandUtil.h"

#include "llvm/Support/TargetSelect.h"

#include "gtest/gtest.h"

namespace llvm::snippy {
struct SnippyTestsEnvironment : public testing::Environment {
  void SetUp() override {
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();
    InitializeAllAsmParsers();
    InitializeAllDisassemblers();
    InitializeAllSnippyTargets();
    RandEngine::init(testing::GTEST_FLAG(random_seed));
  }
};
} // namespace llvm::snippy

int main(int argc, char **argv) {
  // check-llvm-unit uses cli args to interact with gtest
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new llvm::snippy::SnippyTestsEnvironment);
  return RUN_ALL_TESTS();
}
