//===-- BurstGram.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Support/YAMLUtils.h"

#include <map>
#include <set>

namespace llvm {

class MCInstrInfo;

namespace snippy {
enum class BurstMode {
  Basic,
  StoreBurst,
  LoadBurst,
  MixedBurst,
  LoadStoreBurst,
  CustomBurst
};

class OpcodeHistogram;

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

  void convertToCustomMode(const OpcodeHistogram &Histogram,
                           const MCInstrInfo &II);
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::BurstMode);
} // namespace llvm
