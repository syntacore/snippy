#ifndef LLVM_TOOLS_SNIPPY_LIB_AArch64_GEN_CONTEXT_H
#define LLVM_TOOLS_SNIPPY_LIB_AArch64_GEN_CONTEXT_H

//===-- TargetGenContext.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Policy.h"
#include "snippy/Target/Target.h"

#include <optional>

#include "AArch64Generated.h"
#include "llvm/ADT/APInt.h"

namespace llvm {
namespace snippy {

class AArch64GeneratorContext : public TargetGenContextInterface {
public:
  AArch64GeneratorContext() {}

  bool hasActiveRVVMode(const MachineBasicBlock &MBB) const { return false; }


private:
  APInt Default = APInt();
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_AArch64_GEN_CONTEXT_H
