//===-- OpcodeCache.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/MapVector.h"
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
public:
  using OpcodeType = unsigned;

private:
  MapVector<OpcodeType, StringRef> Opnames;
  MapVector<OpcodeType, const MCInstrDesc *> Opdescs;
  MapVector<StringRef, OpcodeType> Opcodes;

public:
  OpcodeCache(const SnippyTarget &Tgt, const MCInstrInfo &II,
              const MCSubtargetInfo &SI);

  const MCInstrDesc *desc(OpcodeType Opcode) const {
    auto Iter = Opdescs.find(Opcode);
    return Iter != Opdescs.end() ? Iter->second : nullptr;
  }

  StringRef name(OpcodeType Opcode) const { return Opnames.lookup(Opcode); }

  void code(const Regex &OpcodeRegexp,
            SmallVectorImpl<unsigned> &Matches) const {
    // TODO: maybe a more efficient solution exists, but no ones knows what it
    // is
    for (const auto &[OpcodeStr, Opcode] : Opcodes)
      if (OpcodeRegexp.match(OpcodeStr))
        Matches.push_back(Opcode);
    std::sort(Matches.begin(), Matches.end());
  }

  unsigned size() const { return Opcodes.size(); }

  std::optional<unsigned> code(StringRef Name) const {
    auto Iter = Opcodes.find(Name);
    return Iter != Opcodes.end() ? std::optional(Iter->second) : std::nullopt;
  }

  void dump(raw_ostream &OS) const {
    for (const auto &[OpName, Opc] : Opcodes)
      OS << format("%10d ", Opc) << OpName << "\n";
  }
};

} // namespace snippy
} // namespace llvm
