//===-- FPUSettings.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/FPUSettings.h"

#include "llvm/Support/YAMLTraits.h"

namespace llvm {

using snippy::FPUSettings;
void yaml::MappingTraits<FPUSettings>::mapping(yaml::IO &IO, FPUSettings &Cfg) {
  IO.mapOptional("overwrite", Cfg.Overwrite);
}

using snippy::FloatOverwriteSettings;
void yaml::MappingTraits<FloatOverwriteSettings>::mapping(
    yaml::IO &IO, FloatOverwriteSettings &Cfg) {
  IO.mapRequired("range", Cfg.Range);
  IO.mapOptional("mode", Cfg.Mode);
}

using snippy::FloatOverwriteRange;
void yaml::MappingTraits<FloatOverwriteRange>::mapping(
    yaml::IO &IO, FloatOverwriteRange &Cfg) {
  IO.mapRequired("max", Cfg.Max);
  IO.mapRequired("min", Cfg.Min);
}

std::string
yaml::MappingTraits<FloatOverwriteRange>::validate(yaml::IO &IO,
                                                   FloatOverwriteRange &Cfg) {
  if (Cfg.Min > Cfg.Max)
    return "Max should be greater or equal than min.";
  return "";
}
using snippy::FloatOverwriteMode;
void yaml::ScalarEnumerationTraits<FloatOverwriteMode>::enumeration(
    yaml::IO &IO, FloatOverwriteMode &M) {
  IO.enumCase(M, "if-all-operands", FloatOverwriteMode::IF_ALL_OPERANDS);
  IO.enumCase(M, "if-any-operand", FloatOverwriteMode::IF_ANY_OPERAND);
}
} // namespace llvm
