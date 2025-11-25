//===-- RegisterAccess.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_CONFIG_REGACCESS_H
#define LLVM_SNIPPY_INCLUDE_CONFIG_REGACCESS_H

#include "snippy/Support/YAMLUtils.h"
#include <unordered_map>

namespace llvm {
namespace snippy {

#ifdef LLVM_SNIPPY_ACCESS_MASKS_BASIC
#error LLVM_SNIPPY_ACCESS_MASKS_BASIC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASKS_BASIC                                         \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SupportR, 0b0001)                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(PrimaryR, 0b0010)                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(R, 0b0011)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SupportW, 0b0100)                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(PrimaryW, 0b1000)                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(W, 0b1100)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SupportRW, 0b0101)                              \
  LLVM_SNIPPY_ACCESS_MASK_DESC(PrimaryRW, 0b1010)                              \
  LLVM_SNIPPY_ACCESS_MASK_DESC(RW, 0b1111)

#ifdef LLVM_SNIPPY_ACCESS_MASKS_ALIAS
#error LLVM_SNIPPY_ACCESS_MASKS_ALIAS is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASKS_ALIAS                                         \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SR, PrimaryR)                                   \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SW, PrimaryW)                                   \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SRW, PrimaryRW)

#define LLVM_SNIPPY_ACCESS_MASKS                                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(None, 0b0000)                                   \
  LLVM_SNIPPY_ACCESS_MASKS_BASIC                                               \
  LLVM_SNIPPY_ACCESS_MASKS_ALIAS

// R - read.
// W - write.
// S - support instruction only.
// G - general instruction only.
enum class AccessMaskBit : uint32_t {
#ifdef LLVM_SNIPPY_ACCESS_MASK_DESC
#error LLVM_SNIPPY_ACCESS_MASK_DESC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASK_DESC(KEY, VALUE) KEY = VALUE,
  LLVM_SNIPPY_ACCESS_MASKS
#undef LLVM_SNIPPY_ACCESS_MASK_DESC
};

template <AccessMaskBit Acc>
constexpr inline StringLiteral AccessMaskNameOf = "";
#define LLVM_SNIPPY_ACCESS_MASK_DESC(NAME, VAL)                                \
  template <>                                                                  \
  constexpr inline StringLiteral AccessMaskNameOf<AccessMaskBit::NAME> =       \
      #NAME;
LLVM_SNIPPY_ACCESS_MASKS_BASIC
#undef LLVM_SNIPPY_ACCESS_MASK_DESC

LLVM_SNIPPY_YAML_STRONG_TYPEDEF(uint32_t, AccessMaskBits);

struct RegisterAccessConfig final : private std::map<unsigned, AccessMaskBit> {
  using map::at;
  using map::begin;
  using map::empty;
  using map::end;
  using map::find;
  using map::size;
  using typename map::iterator;
  using typename map::value_type;
  using map::operator[];
  using map::insert;
  using map::try_emplace;
};

} // namespace snippy
} // namespace llvm

LLVM_SNIPPY_YAML_DECLARE_CUSTOM_MAPPING_TRAITS(
    llvm::snippy::RegisterAccessConfig);

#endif // #ifdef LLVM_SNIPPY_INCLUDE_CONFIG_REGACCESS_H
