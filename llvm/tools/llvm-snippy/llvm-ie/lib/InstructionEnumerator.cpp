//===--- InstructionEnumerator.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InstructionEnumerator.h"

using namespace llvm_ie;

std::unordered_map<InstructionEnumerator::PluginID,
                   InstructionEnumerator::PluginCreateCallback>
    InstructionEnumerator::s_registered_plugins;
