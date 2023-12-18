//===-- OpcodeGenerator.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Support/raw_ostream.h"

#include "RandUtil.h"

#include <map>
#include <random>
#include <vector>

namespace llvm {
namespace snippy {

struct OpcodeGeneratorInterface {
  virtual void print(llvm::raw_ostream &OS) const = 0;
  virtual void dump() const = 0;
  virtual unsigned generate() = 0;
  virtual ~OpcodeGeneratorInterface() {}
};

using OpcGenHolder = std::unique_ptr<OpcodeGeneratorInterface>;

class DefaultOpcodeGenerator final : public OpcodeGeneratorInterface {
  using OpcodeList = std::vector<unsigned>;
  using Distribution = std::discrete_distribution<size_t>;

  OpcodeList Opcodes;
  Distribution OpcodeDist;

public:
  template <typename Iter> DefaultOpcodeGenerator(Iter First, Iter Last) {
    auto Count = std::distance(First, Last);
    if (Count == 0)
      report_fatal_error(
          "OpcodeGenerator initialization failure: empty histogram specified.\n"
          "Usually this may happen when in some context snippy can not find "
          "any instruction that could be created in current context.\n"
          "Try to increase instruction number by one or add more instructions "
          "to "
          "histogram.",
          false);
    std::vector<double> OpcodeWeights(Count);
    std::transform(First, Last, std::back_inserter(Opcodes),
                   [](const auto &HEntry) { return HEntry.first; });
    std::transform(First, Last, OpcodeWeights.begin(),
                   [](const auto &HEntry) { return HEntry.second; });
    if (std::all_of(OpcodeWeights.begin(), OpcodeWeights.end(),
                    [](auto W) { return W == 0; }))
      report_fatal_error("OpcodeGenerator initialization failure: all given to "
                         "histogram opcodes have zero weight",
                         false);
    OpcodeDist = std::discrete_distribution<size_t>(OpcodeWeights.begin(),
                                                    OpcodeWeights.end());
  }

  unsigned generate() override {
    auto Idx = OpcodeDist(RandEngine::engine());
    assert(Idx < Opcodes.size());
    return Opcodes[Idx];
  }

  const Distribution &getDistribution() const { return OpcodeDist; }

  const OpcodeList &getOpcodesList() const { return Opcodes; }

  std::map<unsigned /* Opcode */, double /* probability */>
  getProbabilities() const {
    std::map<unsigned, double> Output;
    const auto OpcodeWeight = zip(Opcodes, OpcodeDist.probabilities());
    for (const auto [Opcode, Weight] : OpcodeWeight) {
      assert(Output.count(Opcode) == 0);
      Output[Opcode] = Weight;
    }
    return Output;
  }

  void print(llvm::raw_ostream &OS) const override;

  void dump() const override { print(dbgs()); }
};

} // namespace snippy
} // namespace llvm
