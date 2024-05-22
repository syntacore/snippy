//===-- SelfcheckInfo.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_SELFCHECK_INFO_H
#define LLVM_TOOLS_SNIPPY_SELFCHECK_INFO_H

#include "snippy/Support/Utils.h"

namespace llvm {
namespace snippy {
struct SelfCheckInfo final {
  unsigned long long CurrentAddress;
  AsOneGenerator<bool, true, false> PeriodTracker;
};
} // namespace snippy
} // namespace llvm
#endif
