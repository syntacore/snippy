//===-- RISCVRegTypes.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {
namespace snippy {

enum class RegType { X, F, V, NoReg };

constexpr inline StringRef regTypeToString(RegType RT) {
  switch (RT) {
  case RegType::X:
    return "X";
  case RegType::F:
    return "F";
  case RegType::V:
    return "V";
  case RegType::NoReg:
    return "NoReg";
  }
  llvm_unreachable("unsupported register type");
}

} // namespace snippy
} // namespace llvm
