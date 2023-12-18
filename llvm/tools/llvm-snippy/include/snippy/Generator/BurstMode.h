//===-- BurstMode.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "snippy/Support/YAMLUtils.h"

namespace llvm {
namespace snippy {

enum class BurstMode {
  Basic,
  StoreBurst,
  LoadBurst,
  MixedBurst,
  LoadStoreBurst,
  CustomBurst
};
} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::BurstMode);
} // namespace llvm
