//===-- OpcodeHistogramVisitor.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/OpcodeHistogramVisitor.h"

namespace llvm {
namespace snippy {

void HistogramVisitor::visit(const NumberNode &) {}

void HistogramVisitor::visit(const HistogramNode &HistNode) {
  HistNode.getArg().accept(*this);
}

void HistogramVisitor::visit(const ChoiceNode &Or) {
  for (auto &&Arg : Or)
    Arg->accept(*this);
}

void HistogramVisitor::visit(const CartesianNode &Mul) {
  for (auto &&Arg : Mul)
    Arg->accept(*this);
}
void HistogramVisitor::visit(const RepeatNode &Pow) {
  auto *ArgNode = Pow.getArg();
  assert(ArgNode);
  ArgNode->accept(*this);
}

OpcodeProbVisitor::OpcodeProbVisitor(const ChoiceNode &StartNode) {
  StartNode.accept(*this);
  calculateOpcodeProbabilities();
}

void OpcodeProbVisitor::reinit(const ChoiceNode &StartNode) {
  OpcWeights.clear();
  NodeWeight.clear();
  StartNode.accept(*this);
  calculateOpcodeProbabilities();
}

void OpcodeProbVisitor::OpcodeProbVisitor::visit(
    const HistogramNode &HistNode) {
  auto *ArgNode = &HistNode.getArg();
  assert(ArgNode);
  NodeWeight[ArgNode] = NodeWeight[&HistNode];
  ArgNode->accept(*this);
}

void OpcodeProbVisitor::visit(const OpcodeNode &OpcNode) {
  auto Opc = OpcNode.getNum();
  // The weight of an individually occurring opcode in the pattern / at the top
  // level of the histogram. Several identical opcodes with different weights
  // may occur.
  auto LocalOpcodeWeight = NodeWeight[&OpcNode];
  OpcWeights[Opc].push_back(LocalOpcodeWeight);
}

void OpcodeProbVisitor::visit(const ChoiceNode &Or) {
  auto *OrParent = Or.getParent();
  // Check if it's a root
  if (!OrParent) {
    NodeWeight[&Or] = BaseNode::DefaultNodeWeight;
    for (auto &&CurrNode : Or) {
      assert(CurrNode);
      NodeWeight[CurrNode.get()] = CurrNode->getWeight();
      CurrNode->accept(*this);
    }
    return;
  }
  auto LevelWeight = Or.getTotalChildsWeight();
  for (auto &&CurrNode : Or) {
    assert(CurrNode);
    assert(CurrNode->getParent() == &Or);
    auto Found = NodeWeight.find(&Or);
    assert(Found != NodeWeight.end() &&
           "All parent nodes must have already been processed");
    auto PathWeight = Found->second;
    // Make normalization for PathWeight
    PathWeight /= LevelWeight;
    auto CurrNodeWeight = PathWeight * CurrNode->getWeight();
    NodeWeight[CurrNode.get()] = CurrNodeWeight;
    CurrNode->accept(*this);
  }
}

OpcodeProbVisitor::OpcProbOpt OpcodeProbVisitor::find(PredType &&Pred) const {
  auto OpcRange = llvm::make_first_range(OpcodeProbs);
  auto Found = llvm::find_if(OpcRange, Pred);
  if (Found == OpcRange.end())
    return std::nullopt;
  auto Opcode = *Found;
  return std::make_pair(Opcode, getProbability(Opcode));
}

double OpcodeProbVisitor::getProbability(unsigned Opcode) const {
  auto Found = OpcodeProbs.find(Opcode);
  if (Found == OpcodeProbs.end())
    return 0.0;
  auto Prob = Found->second;
  assert(Prob <= 1.0);
  return Prob;
}

const OpcodeProbVisitor::OpcodeProbsType &
OpcodeProbVisitor::opcodeProbabilities() const {
  return OpcodeProbs;
}

void OpcodeProbVisitor::calculateOpcodeProbabilities() {
  OpcodeProbs.clear();
  llvm::transform(OpcWeights, std::inserter(OpcodeProbs, OpcodeProbs.end()),
                  [](auto &&OpcToWeights) {
                    auto &Weights = OpcToWeights.second;
                    auto OpcWeight = std::accumulate(
                        Weights.begin(), Weights.end(), /* init */ 0.0);
                    return std::make_pair(OpcToWeights.first, OpcWeight);
                  });
  auto Weights = llvm::make_second_range(OpcodeProbs);
  TotalWeight = std::accumulate(Weights.begin(), Weights.end(), /* init */ 0.0);
  for (auto &&Weight : Weights)
    Weight /= TotalWeight;
}

void OpcodeProbVisitor::visit(const CartesianNode &Mul) {
  assert(Mul.size() >= 2);
  auto *Parent = &Mul;
  for (auto &&CurrNode : Mul) {
    assert(CurrNode);
    assert(CurrNode->getParent() == Parent);
    auto Found = NodeWeight.find(Parent);
    assert(Found != NodeWeight.end() &&
           "All parent nodes must have already been processed");
    auto PathWeight = Found->second;
    NodeWeight[CurrNode.get()] = PathWeight;
    CurrNode->accept(*this);
  }
}

void OpcodeProbVisitor::visit(const RepeatNode &Pow) {
  auto *Arg = Pow.getArg();
  assert(Arg);
  auto Found = NodeWeight.find(&Pow);
  assert(Found != NodeWeight.end() &&
         "All parent nodes must have already been processed");
  auto PathWeight = Found->second;
  NodeWeight[Arg] = PathWeight * Pow.degree();
  Arg->accept(*this);
}

} // namespace snippy
} // namespace llvm
