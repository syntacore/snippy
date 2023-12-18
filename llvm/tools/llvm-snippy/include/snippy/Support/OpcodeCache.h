//===-- OpcodeCache.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/SmallString.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>

namespace llvm {

class MCInstrDesc;

namespace snippy {

class SnippyTarget;

class OpcodeCache {
  // mapping opcode -> name
  std::unordered_map<unsigned, std::string> Opnames;

  // mapping opcode -> desc
  std::unordered_map<unsigned, const MCInstrDesc *> Opdescs;

  // mapping name -> opcode
  std::unordered_map<std::string, unsigned> Opcodes;

public:
  OpcodeCache(const SnippyTarget &Tgt, const MCInstrInfo &II,
              const MCSubtargetInfo &SI);

  const MCInstrDesc *desc(unsigned Opcode) const {
    auto Iter = Opdescs.find(Opcode);
    return Iter != Opdescs.end() ? Iter->second : nullptr;
  }

  std::string_view name(unsigned Opcode) const {
    auto Iter = Opnames.find(Opcode);
    assert(Iter != Opnames.end());
    return Iter->second;
  }

  void code(const Regex &OpcodeRegexp,
            SmallVectorImpl<unsigned> &Matches) const {
    // TODO: maybe a more efficient solution exists, but no ones knows what it
    // is
    for (const auto &[OpcodeStr, Opcode] : Opcodes)
      if (OpcodeRegexp.match(OpcodeStr))
        Matches.push_back(Opcode);
  }

  std::optional<unsigned> code(const std::string &Name) const {
    auto Iter = Opcodes.find(Name);
    return Iter != Opcodes.end() ? std::optional(Iter->second) : std::nullopt;
  }

  void dump() const {
    std::vector<unsigned> VOp;
    VOp.reserve(Opnames.size());
    std::transform(Opnames.begin(), Opnames.end(), std::back_inserter(VOp),
                   [](auto Pair) { return Pair.first; });
    std::sort(VOp.begin(), VOp.end());

    for (const auto &Opn : VOp)
      outs() << format("%10d ", Opn) << Opnames.find(Opn)->second << "\n";
  }
};

} // namespace snippy
} // namespace llvm
