//===-- OpcodeHistogramVisitor.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMVISITOR_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMVISITOR_H

#include "llvm/ADT/DenseMap.h"

#include "snippy/Config/OpcodeHistogramNode.h"

#include <functional>
#include <map>
#include <optional>

namespace llvm {
namespace snippy {

class HistogramVisitor {
public:
  virtual ~HistogramVisitor() = default;

  virtual void visit(const NumberNode &NumNode);
  virtual void visit(const HistogramNode &HistNode);
  virtual void visit(const ChoiceNode &Or);
  virtual void visit(const CartesianNode &Mul);
  virtual void visit(const RepeatNode &Pow);
  virtual void visit(const OpcodeNode &OpcNode) = 0;

protected:
  void acceptArgNodes(const detail::CompositeNode &BinOpNode);
};

// This class is used to correctly calculate opcode probabilities based on the
// provided tree (ChoiceNode). Since OpcodeHistogram gained the ability to
// include patterns, the probability of generating a specific opcode is no
// longer simply OpcodeWeight / TotalOpcodeWeights. This is because the same
// opcode can appear both as a standalone (top-level) opcode and as part of a
// pattern alongside other opcodes. Config example:
//
// histogram-patterns:
//   - AddSub: "ADD | SUB"
// histogram:
//   - [pattern: AddSub, 1.0] # pattern
//   - [ADD, 1.0] # top-level opcode
//
// In the example above, the probability of generating ADD can no longer be
// calculated as 1.0 / 2.0. Instead, we first recalculate the weights of all
// opcodes as follows:
//  -  Weight of ADD = 1.0 + 1.0 / 2 = 1.5
//  -  Weight of SUB = 1.0 / 2 = 0.5
//  -  Total weight = 1.5 + 0.5 = 2.0
// Thus, the final probabilities are:
// ADD: 1.5 / 2.0 = 0.75
// SUB: 0.5 / 2.0 = 0.25
class OpcodeProbVisitor : public HistogramVisitor {
public:
  using PredType = std::function<bool(unsigned)>;
  using OpcodeProbsType =
      std::map</* opcode */ unsigned, /* probability */ double>;
  // Opcode to probability
  using OpcProbPair = OpcodeProbsType::value_type;
  using OpcProbOpt = std::optional<OpcProbPair>;

  OpcodeProbVisitor() = default;
  OpcodeProbVisitor(const ChoiceNode &StartNode);
  // Reinitializes the visitor's internal state based on the provided StartNode
  // while traversing the tree
  void reinit(const ChoiceNode &StartNode);

  OpcProbOpt find(PredType &&Pred) const;

  OpcProbOpt find(unsigned OpcToFind) const {
    return find([OpcToFind](unsigned Opc) { return OpcToFind == Opc; });
  }

  bool contains(unsigned Opcode) const { return OpcWeights.count(Opcode); }

  double getProbability(unsigned Opcode) const;
  // To get a set of opcode-probability pairs
  const OpcodeProbsType &opcodeProbabilities() const;

  double getTotalWeight() const { return TotalWeight; }

private:
  void visit(const HistogramNode &HistNode) override;
  void visit(const OpcodeNode &OpcNode) override;
  void visit(const ChoiceNode &Or) override;
  void visit(const CartesianNode &Mul) override;
  void visit(const RepeatNode &Pow) override;

  void calculateOpcodeProbabilities();

  DenseMap<unsigned, SmallVector<double>> OpcWeights;
  DenseMap<const BaseNode *, double> NodeWeight;
  OpcodeProbsType OpcodeProbs;
  double TotalWeight;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMVISITOR_H
