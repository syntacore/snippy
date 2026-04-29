//===-- SMCManager.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SMCManager.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Target/Target.h"

namespace llvm {
namespace snippy {

std::vector<unsigned> SMCManagerT::getOrCreateSMCRegList(
    planning::InstructionGenerationContext &IGC) {
  if (SMCRegList.size())
    return SMCRegList;

  auto &ProgCtx = IGC.ProgCtx;
  const auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  SMCRegList = Tgt.getRegListForMemCpyForSMC(IGC);
  return SMCRegList;
}

} // namespace snippy
} // namespace llvm
