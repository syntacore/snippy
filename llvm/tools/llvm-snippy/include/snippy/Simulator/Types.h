//===-- Types.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TYPES_H
#define LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TYPES_H

#include "llvm/ADT/APInt.h"

namespace llvm {
namespace snippy {

using ProgramCounterType = uint64_t;
using MemoryAddressType = uint64_t;
using RegisterType = uint64_t;
using VectorRegisterType = APInt;
// Currently only instructions up to 32 bits are supported.
using MaxInstrBitsType = uint32_t;

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TYPES_H
