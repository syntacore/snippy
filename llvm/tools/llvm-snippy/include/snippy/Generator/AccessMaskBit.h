#ifndef LLVM_TOOLS_SNIPPY_ACCESS_MASK_BIT_H
#define LLVM_TOOLS_SNIPPY_ACCESS_MASK_BIT_H

//===-- AccessMaskBit.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

namespace llvm {
namespace snippy {

#ifdef LLVM_SNIPPY_ACCESS_MASKS
#error LLVM_SNIPPY_ACCESS_MASKS is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASKS                                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(None, 0b0000)                                   \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SR, 0b0001)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GR, 0b0010)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(R, 0b0011)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SW, 0b0100)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GW, 0b1000)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(W, 0b1100)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SRW, 0b0101)                                    \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GRW, 0b1010)                                    \
  LLVM_SNIPPY_ACCESS_MASK_DESC(RW, 0b1111)

// R - read.
// W - write.
// S - support instruction only.
// G - general instruction only.
enum class AccessMaskBit {
#ifdef LLVM_SNIPPY_ACCESS_MASK_DESC
#error LLVM_SNIPPY_ACCESS_MASK_DESC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASK_DESC(KEY, VALUE) KEY = VALUE,
  LLVM_SNIPPY_ACCESS_MASKS
#undef LLVM_SNIPPY_ACCESS_MASK_DESC
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_ACCESS_MASK_BIT_H
