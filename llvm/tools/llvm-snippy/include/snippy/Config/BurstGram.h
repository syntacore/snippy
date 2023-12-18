//===-- BurstGram.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Generator/BurstMode.h"
#include "snippy/Support/YAMLUtils.h"

#include <map>
#include <set>

namespace llvm {
namespace snippy {

struct BurstGramData final {
  BurstMode Mode = BurstMode::Basic;
  unsigned MinSize = 0;
  unsigned MaxSize = 0;
  using UniqueOpcodesTy = std::set<unsigned>;
  using GroupingsTy = std::vector<UniqueOpcodesTy>;
  std::optional<GroupingsTy> Groupings = std::nullopt;
  using OpcodeToNumGroupsTy = std::map<unsigned, unsigned>;
  // Returns a mapping from opcode to the number of burst groups it is used in.
  OpcodeToNumGroupsTy getOpcodeToNumBurstGroups() const {
    std::map<unsigned, unsigned> OpcodeToNumOfGroups;
    for (const auto &Group : *Groupings)
      for (auto Opcode : Group)
        ++OpcodeToNumOfGroups[Opcode];

    return OpcodeToNumOfGroups;
  }

  UniqueOpcodesTy getAllBurstOpcodes() const {
    UniqueOpcodesTy Opcodes;
    if (!Groupings)
      return Opcodes;
    for (const auto &GroupOpcodes : *Groupings)
      Opcodes.insert(GroupOpcodes.begin(), GroupOpcodes.end());
    return Opcodes;
  }
};

struct BurstGram final {
  std::optional<BurstGramData> Data;

  operator bool() const { return Data.has_value(); }
};

} // namespace snippy
} // namespace llvm
