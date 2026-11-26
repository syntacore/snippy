//===-- BaseRegisterGroups.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Base-register groups shared by burst and patterns. Opcodes that belong to
// different groups are never allowed to share a base register.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BASEREGISTERGROUPS_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BASEREGISTERGROUPS_H

#include "snippy/Support/YAMLUtils.h"

#include <optional>
#include <set>
#include <vector>

namespace llvm {
namespace snippy {

class OpcodeCache;

// A single group of opcodes that share a base register.
using UniqueOpcodesTy = std::set<unsigned>;
// All base-register groups.
using OpcodeGroupsTy = std::vector<UniqueOpcodesTy>;
// The type of a "base-register-groups" configuration value.
using BaseRegisterGroupsTy = OpcodeGroupsTy;

UniqueOpcodesTy opcodeGroupFromNames(const SList &Names,
                                     const OpcodeCache &OpCC);

// Parses a "base-register-groups" configuration value into the denormalized
// opcode-group representation.
// FIXME: waiting for c++20
// template<ranges::input_range RangeType>
template <typename RangeType>
std::optional<BaseRegisterGroupsTy>
parseBaseRegisterGroups(const std::vector<SList> &RawGroups,
                        const OpcodeCache &OpCC,
                        const RangeType &AllPossibleOpcodes) {
  if (RawGroups.empty())
    return std::nullopt;

  BaseRegisterGroupsTy Denorm;
  Denorm.reserve(RawGroups.size());
  llvm::transform(
      RawGroups, std::back_inserter(Denorm),
      [&OpCC](const auto &Names) { return opcodeGroupFromNames(Names, OpCC); });

  auto IsNotContainedInAnyGroup = [&Denorm](auto Opcode) {
    return all_of(
        Denorm, [&Opcode](const auto &Group) { return !Group.count(Opcode); });
  };
  auto NotSpecifiedOpcodes =
      make_filter_range(AllPossibleOpcodes, IsNotContainedInAnyGroup);
  // Any opcode not listed in an explicit group lands in a single implicit
  // "default" group so that it still gets a bound base register.
  Denorm.emplace_back(NotSpecifiedOpcodes.begin(), NotSpecifiedOpcodes.end());
  return Denorm;
}

// Converts `Groups` (sets of internal opcodes) back into the denormalized
// representation (lists of opcode names) so that they can be printed out.
std::vector<SList> denormalizeBaseRegisterGroups(const OpcodeGroupsTy &Groups,
                                                 const OpcodeCache &OpCC);

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BASEREGISTERGROUPS_H
