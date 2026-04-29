//===-- SMCGram.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Simulator/Types.h"
#include "snippy/Support/YAMLUtils.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
namespace snippy {

struct SMCGram final {
  static constexpr double DefaultSMCTgtBlocksRatio = 0.5;
  static constexpr StringLiteral ImmediateSMCMode = "immediate";

  enum class SMCMode { Immediate };

  double SMCTgtBlocksRatio = DefaultSMCTgtBlocksRatio;
  NumericRange<unsigned> SMCOverwriters = {1, 1};
  SMCMode Mode = SMCMode::Immediate;

  void print(raw_ostream &OS) const;
};

template <SMCGram::SMCMode M> constexpr StringLiteral getSMCModeName();

template <>
inline constexpr StringLiteral getSMCModeName<SMCGram::SMCMode::Immediate>() {
  return SMCGram::ImmediateSMCMode;
}

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::SMCGram);
} // namespace llvm
