//===-- FPUSettings.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_CONFIG_FPU_SETTINGS_H
#define LLVM_SNIPPY_INCLUDE_CONFIG_FPU_SETTINGS_H

#include "snippy/Support/YAMLUtils.h"

namespace llvm {
namespace snippy {

struct FloatOverwriteRange final {
  int Min;
  int Max;
};

enum class FloatOverwriteMode {
  IF_ANY_OPERAND,
  IF_ALL_OPERANDS,
};

struct FloatOverwriteSettings final {
  FloatOverwriteRange Range;
  FloatOverwriteMode Mode = FloatOverwriteMode::IF_ALL_OPERANDS;
};

struct FPUSettings final {
  std::optional<FloatOverwriteSettings> Overwrite;
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::FPUSettings);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::FloatOverwriteSettings);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::FloatOverwriteRange);

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::FloatOverwriteMode);
} // namespace llvm

#endif
