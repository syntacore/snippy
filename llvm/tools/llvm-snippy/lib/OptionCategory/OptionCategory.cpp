//===-- OptionCategory.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Definition of llvm-snippy option category.
///
//===----------------------------------------------------------------------===//
#include "llvm/Support/CommandLine.h"

namespace llvm::snippy {

cl::OptionCategory Options("llvm-snippy options");

}
