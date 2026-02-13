//===-- TargetGenContext.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetGenContext.h"
#include "RISCVSubtarget.h"

namespace llvm::snippy {

RISCVGeneratorContext::RISCVGeneratorContext(
    RISCVConfigurationInfo &&RISCVConfigIn,
    DenseSet<unsigned> OpcodesWithNonIntersectingMemAccesses)
    : RISCVConfig(std::move(RISCVConfigIn)),
      DisallowedIntersectingMemAccessesForOpcodes(
          std::move(OpcodesWithNonIntersectingMemAccesses)) {}

bool RISCVGeneratorContext::disallowIntersectingMemoryAccesses(
    unsigned Opcode) const {
  return DisallowedIntersectingMemAccessesForOpcodes.contains(Opcode);
}

} // namespace llvm::snippy
