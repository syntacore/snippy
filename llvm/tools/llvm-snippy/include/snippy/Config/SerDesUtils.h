//===-- SerDesUtils.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_LIB_SERDES_UTILS_H
#define LLVM_TOOLS_SNIPPY_LIB_SERDES_UTILS_H

#include "ImmediateHistogram.h"
#include "RegisterHistogram.h"

#include "llvm/ADT/APFloat.h"

namespace llvm::snippy {

struct RegisterSerialization {
  RegisterValues Registers;

  RegisterSerialization &addRegisterGroup(StringRef Prefix,
                                          ArrayRef<uint64_t> Values);

  RegisterSerialization &addRegisterGroup(StringRef Prefix, unsigned BitsNum,
                                          ArrayRef<APInt> Values);

  void saveAsYAML(raw_ostream &OS);
};

RegistersWithHistograms loadRegistersFromYaml(StringRef Path);

} // namespace llvm::snippy

#endif // LLVM_TOOLS_SNIPPY_LIB_SERDES_UTILS_H
