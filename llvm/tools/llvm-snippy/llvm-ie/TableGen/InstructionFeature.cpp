//===--- InstructionFeature.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <string>
#include <string_view>

#include "llvm/ADT/STLExtras.h"
#include "llvm/TableGen/Record.h"

#include "InstructionFeature.h"

using namespace instr_enumerator_tblgen;

static std::vector<std::string>
filterByFeature(const std::vector<const llvm::Record *> &Instrs,
                InstructionFeature::FeatureFilter Filter) {
  std::vector<const llvm::Record *> FilteredInstrs;
  llvm::copy_if(Instrs, std::back_inserter(FilteredInstrs),
                [Filter](const llvm::Record *Instr) { return Filter(Instr); });

  std::vector<std::string> InstrNames;
  InstrNames.reserve(FilteredInstrs.size());
  llvm::transform(FilteredInstrs, std::back_inserter(InstrNames),
                  [](const llvm::Record *Instr) -> std::string {
                    return Instr->getName().str();
                  });

  return InstrNames;
}

namespace instr_enumerator_tblgen {

void emitFeatures(const std::vector<const llvm::Record *> &Instrs,
                  const std::vector<InstructionFeature> &Features,
                  llvm::raw_ostream &OS) {
  OS << "static const llvm::DenseMap<llvm::StringRef, "
        "std::set<llvm::StringRef>> FeatureTable = {\n";

  for (auto &&Feature : Features) {
    OS << "{\"" << Feature.Name << "\", {";
    for (auto &&Instr : filterByFeature(Instrs, Feature.Filter))
      OS << "\"" << Instr << "\", ";
    for (auto &&Instr : Feature.AdditionalInstrs)
      OS << "\"" << Instr << "\", ";
    OS << "}},\n";
  }

  OS << "};\n";
}

bool memoryAccessFeatureFilter(const llvm::Record *Instr) {
  bool Unset = false;
  return Instr->getValueAsBitOrUnset("mayLoad", Unset) ||
         Instr->getValueAsBitOrUnset("mayStore", Unset);
}

bool controlFlowFeatureFilter(const llvm::Record *Instr) {
  return Instr->getValueAsBit("isReturn") || Instr->getValueAsBit("isBranch") ||
         Instr->getValueAsBit("isCall") ||
         Instr->getValueAsBit("isTerminator") ||
         Instr->getValueAsBit("isIndirectBranch");
}

} // namespace instr_enumerator_tblgen
