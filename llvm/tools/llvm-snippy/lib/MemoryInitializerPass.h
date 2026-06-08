//===-- MemoryInitializerPass.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_LIB_MEMORYINITIALIZERPASS_H
#define LLVM_TOOLS_LLVM_SNIPPY_LIB_MEMORYINITIALIZERPASS_H

#include "llvm/Pass.h"
namespace llvm {
namespace snippy {
class MemoryInitializer final : public ModulePass {
  // Shows that memory will be initialized with __snippy_random before snippet
  bool ExternalCallOfMemInitRoutine = false;

public:
  static char ID;

  MemoryInitializer(bool ExternalCallOfMemInitRoutine = false)
      : ModulePass(ID),
        ExternalCallOfMemInitRoutine{ExternalCallOfMemInitRoutine} {}

  StringRef getPassName() const override;

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_LIB_MEMORYINITIALIZERPASS_H
