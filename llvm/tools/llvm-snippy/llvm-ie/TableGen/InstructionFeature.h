//===--- InstructionFeature.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_TABLEGEN_INSTRUCTION_FEATURE_H
#define LLVM_TOOLS_LLVM_IE_TABLEGEN_INSTRUCTION_FEATURE_H

namespace instr_enumerator_tblgen {

struct InstructionFeature {
  using FeatureFilter = typename std::function<bool(const llvm::Record *)>;

  explicit InstructionFeature(std::string_view FeatureName)
      : Name{FeatureName} {}

  InstructionFeature &specifyFilter(FeatureFilter Callback) {
    Filter = Callback;
    return *this;
  }

  template <typename... Args> InstructionFeature &addInstrs(Args &&...Instrs) {
    (AdditionalInstrs.push_back(std::forward<Args>(Instrs)), ...);
    return *this;
  }

  template <typename Range> InstructionFeature &addInstrs(Range &&Instrs) {
    llvm::copy(std::forward<Range>(Instrs),
               std::back_inserter(AdditionalInstrs));
    return *this;
  }

  std::string Name;
  FeatureFilter Filter;
  std::vector<std::string> AdditionalInstrs;
};

void emitFeatures(const std::vector<const llvm::Record *> &Instrs,
                  const std::vector<InstructionFeature> &Feature,
                  llvm::raw_ostream &OS);

bool memoryAccessFeatureFilter(const llvm::Record *Instr);

bool controlFlowFeatureFilter(const llvm::Record *Instr);

} // namespace instr_enumerator_tblgen

#endif
