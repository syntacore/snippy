//===-- OpcodeCache.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/OpcodeCache.h"
#include "snippy/Target/Target.h"

#include "llvm/MC/MCInstrInfo.h"

namespace llvm {
namespace snippy {

OpcodeCache::OpcodeCache(const SnippyTarget &Tgt, const MCInstrInfo &II,
                         const MCSubtargetInfo &SI) {
  for (unsigned Opcode = 1, E = II.getNumOpcodes(); Opcode < E; ++Opcode) {
    const MCInstrDesc &InstrDesc = II.get(Opcode);
    if (InstrDesc.isPseudo())
      continue;
    if (!Tgt.checkOpcodeSupported(Opcode, SI))
      continue;

    // support all mappings
    Opnames[Opcode] = II.getName(Opcode).str();
    Opdescs[Opcode] = &II.get(Opcode);
    Opcodes[Opnames[Opcode]] = Opcode;
  }
}

} // namespace snippy
} // namespace llvm
