//===------- AddMetadataSectionPass.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Config/llvm-config.h"
#include "llvm/Pass.h"

namespace llvm {

class ModulePass;
class PassRegistry;

ModulePass *createAddMetadataSectionPass();

void initializeAddMetadataSectionPass(PassRegistry &Registry);

} // namespace llvm
