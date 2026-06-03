//===-- OpcodeGenerator.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

namespace llvm {
namespace snippy {

struct OpcodeGeneratorInterface {
  virtual void print(llvm::raw_ostream &OS) const = 0;
  virtual void dump() const = 0;
  virtual void generate(SmallVectorImpl<unsigned> &Opcodes) = 0;
  virtual std::unique_ptr<OpcodeGeneratorInterface> copy() const = 0;
  virtual ~OpcodeGeneratorInterface() {}
};

using OpcGenHolder = std::unique_ptr<OpcodeGeneratorInterface>;

class DefaultOpcodeGenerator final : public OpcodeGeneratorInterface {
  using OpcodeProbsType = OpcodeProbVisitor::OpcodeProbsType;

  OpcodeHistogram OpcodeHist;
  OpcodeProbsType OpcodeProbs;

public:
  DefaultOpcodeGenerator(const OpcodeHistogram &OpcHist)
      : OpcodeHist(OpcHist), OpcodeProbs(OpcodeHist.opcodeProbabilities()) {
    if (OpcodeHist.size() == 0)
      snippy::fatal(
          "OpcodeGenerator initialization failure: empty histogram specified.\n"
          "Usually this may happen when in some context snippy can not find "
          "any instruction that could be created in current context.\n"
          "Try to increase instruction number by one or add more instructions "
          "to "
          "histogram.");
    auto Probs = llvm::make_second_range(OpcodeProbs);
    if (llvm::all_of(Probs, [](double W) { return W == 0.0; }))
      snippy::fatal("OpcodeGenerator initialization failure: all given to "
                    "histogram opcodes have zero weight");
  }

  std::unique_ptr<OpcodeGeneratorInterface> copy() const override {
    return std::make_unique<DefaultOpcodeGenerator>(*this);
  }

  void generate(SmallVectorImpl<unsigned> &Opcodes) override;

  auto getOpcodesList() const { return llvm::make_first_range(OpcodeProbs); }

  void print(llvm::raw_ostream &OS) const override;

  void dump() const override { print(dbgs()); }
};

// We call this function when we are absolutely sure that exactly one opcode
// will always be generated
template <typename OpcodeGeneratorType>
unsigned generateSingleOpcode(OpcodeGeneratorType &&OpcGen) {
  SmallVector<unsigned, 1> OpcSeq;
  std::forward<OpcodeGeneratorType>(OpcGen).generate(OpcSeq);
  assert(OpcSeq.size() == 1);
  return OpcSeq.front();
}

} // namespace snippy
} // namespace llvm
