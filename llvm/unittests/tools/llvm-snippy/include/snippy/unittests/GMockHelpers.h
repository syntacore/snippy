//===-- GMockHelpers.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains helper functions for snippy unittests.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_GMOCK_HELPERS_H
#define LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_GMOCK_HELPERS_H

#include "gmock/gmock-matchers.h"

#include <string>

namespace llvm::snippy::testing {

template <typename M, typename V, typename Listener>
bool explainInnerMatchResult(const M &Matcher, const V &Value,
                             const std::string &Name, Listener *L) {
  using namespace ::testing;
  StringMatchResultListener LocalListener;

  if (!ExplainMatchResult(Matcher, Value, &LocalListener)) {
    *L << "\n  where the " << Name << " doesn't match";

    std::string Explanation = LocalListener.str();
    if (!Explanation.empty()) {
      *L << ": " << Explanation;
    }
    return false;
  }
  return true;
}

} // namespace llvm::snippy::testing

#endif // LLVM_UNITTESTS_TOOLS_LLVM_SNIPPY_GMOCK_HELPERS_H