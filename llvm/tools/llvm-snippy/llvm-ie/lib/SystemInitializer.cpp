//===--- SystemInitializer.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SystemInitializer.h"
#include "Plugins.h"

#define LLVM_IE_TARGET(p) LLVM_IE_TARGET_DECLARE(p)
#include "Targets.def"

namespace llvm_ie {

void initialize() {

#define LLVM_IE_TARGET(p) LLVM_IE_TARGET_INITIALIZE(p)
#include "Targets.def"
}

void terminate() {

#define LLVM_IE_TARGET(p) LLVM_IE_TARGET_TERMINATE(p)
#include "Targets.def"
}

} // namespace llvm_ie
