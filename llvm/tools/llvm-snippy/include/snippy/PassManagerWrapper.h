//===-- PassManagerWrapper.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef PASS_MANAGER_WRAPPER_H_
#define PASS_MANAGER_WRAPPER_H_

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/CodeGen.h"

namespace llvm {

class LLVMTargetMachine;
class raw_pwrite_stream;
class MCContext;

namespace snippy {

class PassManagerWrapper final {
  legacy::PassManager PM;

public:
  void add(Pass *P);
  bool addAsmPrinter(LLVMTargetMachine &LLVMTM, raw_pwrite_stream &Out,
                     raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
                     MCContext &Context);
  bool run(Module &M) { return PM.run(M); };
  auto &getPM() & { return PM; }
  const auto &getPM() const & { return PM; }
};

} // namespace snippy
} // namespace llvm

#endif // PASS_MANAGER_WRAPPER_H_
