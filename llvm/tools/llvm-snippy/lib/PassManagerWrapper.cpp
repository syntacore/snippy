//===-- PassManagerWrapper.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/PassManagerWrapper.h"

#include "llvm/Target/TargetMachine.h"

namespace llvm {
namespace snippy {

void PassManagerWrapper::add(Pass *P) { PM.add(P); }

bool PassManagerWrapper::addAsmPrinter(LLVMTargetMachine &LLVMTM,
                                       raw_pwrite_stream &Out,
                                       raw_pwrite_stream *DwoOut,
                                       CodeGenFileType FileType,
                                       MCContext &Context) {
  return LLVMTM.addAsmPrinter(PM, Out, DwoOut, FileType, Context);
}

} // namespace snippy
} // namespace llvm
