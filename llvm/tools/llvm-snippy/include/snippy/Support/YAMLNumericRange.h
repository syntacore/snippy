//===-- YAMLNumericRange.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/YAMLTraits.h"

namespace llvm {

template <typename T> struct yaml::MappingTraits<snippy::NumericRange<T>> {
  static void mapping(yaml::IO &IO, snippy::NumericRange<T> &Range) {
    IO.mapOptional("min", Range.Min);
    IO.mapOptional("max", Range.Max);
  }

  static std::string validate(yaml::IO &IO, snippy::NumericRange<T> &Range) {
    if (Range.Min && Range.Max && *Range.Min > *Range.Max)
      return "'min' expected to be less or equal 'max'";
    return {};
  }
};

} // namespace llvm
