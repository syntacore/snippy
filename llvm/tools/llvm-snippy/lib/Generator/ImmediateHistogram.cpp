//===-- ImmediateHistogram.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/ImmediateHistogram.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "snippy-opcode-to-immediate-histogram-map"

namespace llvm {
namespace snippy {

OpcodeToImmHistSequenceMap::OpcodeToImmHistSequenceMap(
    const ImmediateHistogramRegEx &ImmHist, const OpcodeHistogram &OpcHist,
    const OpcodeCache &OpCC) {
  unsigned NMatched = 0;
  for (auto Opc : make_first_range(OpcHist)) {
    for (auto &&Conf : ImmHist.Exprs) {
      if (NMatched == OpcHist.size()) {
        LLVMContext Ctx;
        snippy::warn(WarningName::InconsistentOptions, Ctx,
                     "Unused regex in immediate histogram",
                     "all opcodes were already matched so no opcodes remained "
                     "to be matched by \"" +
                         Twine(Conf.Expr) + "\".");
        break;
      }
      Regex RX(Conf.Expr);
      auto Name = OpCC.name(Opc);
      if (RX.match(Name)) {
        auto Inserted = Data.emplace(Opc, Conf.Data);
        if (Inserted.second) {
          ++NMatched;
          LLVM_DEBUG(dbgs() << "Immediate Histogram matched opcode \"" << Name
                            << "\" with regex \"" << Conf.Expr << "\".\n");
        }
      }
    }
    // Uniform by default.
    auto Inserted = Data.emplace(Opc, ImmHistOpcodeSettings());
    if (Inserted.second) {
      LLVMContext Ctx;
      snippy::notice(WarningName::NotAWarning, Ctx,
                     "No regex that matches \"" + Twine(OpCC.name(Opc)) +
                         "\" was found in immediate histogram",
                     "Uniform destribution will be used.");
    }
  }
}

} // namespace snippy
} // namespace llvm
