//===--- CommandLineOpts.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_LIB_COMMAND_LINE_OPTS_H
#define LLVM_TOOLS_LLVM_IE_LIB_COMMAND_LINE_OPTS_H

#include "llvm/Support/CommandLine.h"

namespace opts {

extern llvm::cl::OptionCategory FeatureOptionsCategory;

extern llvm::cl::opt<bool> MemoryAccessOpt;
extern llvm::cl::opt<bool> ControlFlowOpt;

} // namespace opts

#endif
