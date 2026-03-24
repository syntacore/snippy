//===-- OpcodeHistogramNode.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/OpcodeHistogramNode.h"
#include "snippy/Config/OpcodeHistogramVisitor.h"

namespace llvm {
namespace snippy {

void NumberNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

void OpcodeNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

SmallVector<double> detail::CompositeNode::getChildWeights() const {
  SmallVector<double> WeightDist;
  WeightDist.reserve(size());
  llvm::transform(ChildNodes, std::back_inserter(WeightDist),
                  [](auto &&ChildNode) {
                    assert(ChildNode);
                    return ChildNode->getWeight();
                  });
  return WeightDist;
}

double detail::CompositeNode::getTotalChildsWeight() const {
  auto Weights = getChildWeights();
  return std::accumulate(Weights.begin(), Weights.end(), /* init */ 0.0);
}

ChoiceNode::ChoiceNode(const ChoiceNode &Rhs) {
  for (auto &&Arg : Rhs.ChildNodes) {
    assert(Arg);
    ChildNodes.push_back(Arg->clone());
    ChildNodes.back()->setParent(this);
  }
  NodeDistOpt = Rhs.NodeDistOpt;
}

ChoiceNode::ChoiceNode(ChoiceNode &&Rhs)
    : CompositeNode(std::move(Rhs)), NodeDistOpt(std::move(Rhs.NodeDistOpt)) {
  for (auto &&Arg : ChildNodes) {
    assert(Arg);
    Arg->setParent(this);
  }
}

ChoiceNode &ChoiceNode::operator=(const ChoiceNode &Rhs) {
  if (this == std::addressof(Rhs))
    return *this;

  auto Tmp = Rhs;
  ChildNodes = std::move(Tmp.ChildNodes);
  NodeDistOpt = std::move(Tmp.NodeDistOpt);
  for (auto &&ChildNode : ChildNodes) {
    assert(ChildNode);
    ChildNode->setParent(this);
  }
  return *this;
}

ChoiceNode &ChoiceNode::operator=(ChoiceNode &&Rhs) {
  ChildNodes = std::move(Rhs.ChildNodes);
  NodeDistOpt = std::move(Rhs.NodeDistOpt);
  for (auto &&ChildNode : ChildNodes) {
    assert(ChildNode);
    ChildNode->setParent(this);
  }
  return *this;
}

void ChoiceNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

ChoiceNode::ResultSequenceType ChoiceNode::evaluateRandChildNode() const {
  assert(NodeDistOpt.has_value());
  auto RandID = NodeDistOpt.value()(RandEngine::engine());
  assert(RandID < ChildNodes.size());
  assert(ChildNodes[RandID]);
  return ChildNodes[RandID]->evaluate();
}

ChoiceNode::ResultSequenceType ChoiceNode::evaluate() const {
  if (!NodeDistOpt.has_value()) {
    auto NodeWeights = getChildWeights();
    NodeDistOpt = std::discrete_distribution<size_t>(NodeWeights.begin(),
                                                     NodeWeights.end());
  }
  return evaluateRandChildNode();
}

CartesianNode::ResultSequenceType CartesianNode::evaluate() const {
  ResultSequenceType ResultSeq;
  for (auto &&NodePtr : ChildNodes) {
    assert(NodePtr);
    auto EvalRes = NodePtr->evaluate();
    llvm::append_range(ResultSeq, EvalRes);
  }
  return ResultSeq;
}

RepeatNode::ResultSequenceType RepeatNode::evaluate() const {
  assert(ArgNode);
  ResultSequenceType ResultSeq;
  Degree = generateDegree();
  for (auto Id = 0u; Id < Degree; ++Id) {
    auto SeqToPow = ArgNode->evaluate();
    assert(!SeqToPow.empty());
    llvm::append_range(ResultSeq, SeqToPow);
  }
  return ResultSeq;
}

void CartesianNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}
void RepeatNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}
void HistogramNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

} // namespace snippy
} // namespace llvm
