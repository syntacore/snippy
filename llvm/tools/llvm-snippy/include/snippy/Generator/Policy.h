//===-- Policy.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Support/OpcodeGenerator.h"

namespace llvm {
namespace snippy {

class GeneratorContext;

struct IGenPolicy {
  virtual unsigned genNextOpc() = 0;
  virtual void print(raw_ostream &OS) const = 0;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
  virtual ~IGenPolicy() = default;
};

using GenPolicy = std::unique_ptr<IGenPolicy>;

class DefaultGenPolicy final : public IGenPolicy {
  OpcGenHolder OpcGen;

public:
  DefaultGenPolicy(const GeneratorContext &SGCtx,
                   std::function<bool(unsigned)> Filter,
                   bool MustHavePrimaryInstrs,
                   ArrayRef<OpcodeHistogramEntry> Overrides);

  unsigned genNextOpc() override { return OpcGen->generate(); }

  void print(raw_ostream &OS) const override {
    OS << "Default Generation Policy\n";
  }
};

class BurstGenPolicy final : public IGenPolicy {
  std::vector<unsigned> Opcodes;
  std::discrete_distribution<size_t> Dist;

public:
  BurstGenPolicy(const GeneratorContext &SGCtx, unsigned BurstGroupID);

  unsigned genNextOpc() override {
    return Opcodes.at(Dist(RandEngine::engine()));
  }

  void print(raw_ostream &OS) const override {
    OS << "Burst Generation Policy: { ";
    for (auto [Opc, Prob] : zip(Opcodes, Dist.probabilities())) {
      OS << Opc << "(" << Prob << ") ";
    }
    OS << "}\n";
  }
};

class FinalInstPolicy final : public IGenPolicy {
  unsigned Opcode;

public:
  FinalInstPolicy(unsigned Opc) : Opcode(Opc) {}

  unsigned genNextOpc() override { return Opcode; }

  void print(raw_ostream &OS) const override {
    OS << "Final Instruction Policy (" << Opcode << ")\n";
  }
};

} // namespace snippy
} // namespace llvm
