//===-- BurstGram.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BURSTGRAM_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BURSTGRAM_H

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
  using OpcodeGroupsTy = std::vector<UniqueOpcodesTy>;
  std::optional<OpcodeGroupsTy> Groupings = std::nullopt;
  std::optional<OpcodeGroupsTy> BaseRegisterGroups = std::nullopt;
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

  void removeUnsupportedOpcodes(LLVMState &State, const OpcodeCache &OpCC);
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::BurstMode);
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_BURSTGRAM_H
