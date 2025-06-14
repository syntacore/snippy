//===- SnippyOptions.h ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_UTILS_TABLEGEN_SNIPPYOPTIONS_H
#define LLVM_TOOLS_LLVM_SNIPPY_UTILS_TABLEGEN_SNIPPYOPTIONS_H

namespace llvm {
class RecordKeeper;
class raw_ostream;

namespace snippy {

bool emitSnippyOptions(llvm::raw_ostream &OS, llvm::RecordKeeper &Records);
bool emitSnippyOptionsStruct(raw_ostream &OS, RecordKeeper &Records);

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_SNIPPY_UTILS_TABLEGEN_SNIPPYOPTIONS_H
