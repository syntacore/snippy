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
namespace {

template <typename ChildStorage>
void setChildsParentLink(ChildStorage &&ChildNodes, const BaseNode *Parent) {
  llvm::for_each(ChildNodes, [Parent](auto &&ChildPtr) {
    assert(ChildPtr);
    ChildPtr->setParent(Parent);
  });
}

template <typename StorageType>
bool compareTwoPtrSequences(const StorageType &Lhs, const StorageType &Rhs) {
  return llvm::equal(Lhs, Rhs, [&](auto &&LhsPtr, auto &&RhsPtr) {
    assert(LhsPtr);
    assert(RhsPtr);
    return *LhsPtr == *RhsPtr;
  });
}

} // namespace

bool NumberNode::compareLess(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<NumberNode>(&Rhs))
    return getNum() < TruePtrNode->getNum();
  return getNodeStorageType() < Rhs.getNodeStorageType();
}

bool NumberNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<NumberNode>(&Rhs))
    return getNum() == TruePtrNode->getNum();
  return false;
}

void NumberNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

bool OpcodeNode::compareLess(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<OpcodeNode>(&Rhs))
    return std::make_tuple(getWeight(), getNum()) <
           std::make_tuple(TruePtrNode->getWeight(), TruePtrNode->getNum());
  return getNodeStorageType() < Rhs.getNodeStorageType();
}

bool OpcodeNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<OpcodeNode>(&Rhs))
    return std::make_tuple(getNum(), getWeight()) ==
           std::make_tuple(TruePtrNode->getNum(), TruePtrNode->getWeight());
  return false;
}

void OpcodeNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

ChoiceNode::ChoiceNode(NodeHandle Arg) {
  assert(Arg);
  insert(std::move(Arg));
  setChildsParentLink(ChildNodes, this);
}

ChoiceNode::ChoiceNode(SmallVector<NodeHandle> Args) {
  for (auto &&ArgNode : Args)
    insert(std::move(ArgNode));
  setChildsParentLink(ChildNodes, this);
}

ChoiceNode::ChoiceNode(const ChoiceNode &Rhs) {
  for (auto &&Arg : Rhs.ChildNodes) {
    assert(Arg);
    auto InsIt = ChildNodes.insert(Arg->clone());
    (*InsIt)->setParent(this);
    ChildIters.push_back(InsIt);
  }
  NodeDistOpt = Rhs.NodeDistOpt;
}

ChoiceNode::ChoiceNode(ChoiceNode &&Rhs)
    : ChildNodes(std::move(Rhs.ChildNodes)),
      ChildIters(std::move(Rhs.ChildIters)),
      NodeDistOpt(std::move(Rhs.NodeDistOpt)) {
  setChildsParentLink(ChildNodes, this);
}

ChoiceNode &ChoiceNode::operator=(const ChoiceNode &Rhs) {
  if (this == std::addressof(Rhs))
    return *this;

  auto Tmp = Rhs;
  ChildNodes = std::move(Tmp.ChildNodes);
  ChildIters = std::move(Tmp.ChildIters);
  NodeDistOpt = std::move(Tmp.NodeDistOpt);
  setChildsParentLink(ChildNodes, this);

  return *this;
}

ChoiceNode &ChoiceNode::operator=(ChoiceNode &&Rhs) {
  ChildNodes = std::move(Rhs.ChildNodes);
  ChildIters = std::move(Rhs.ChildIters);
  NodeDistOpt = std::move(Rhs.NodeDistOpt);
  setChildsParentLink(ChildNodes, this);

  return *this;
}

void ChoiceNode::insert(NodeHandle ArgNode) {
  assert(ArgNode);
  ArgNode->setParent(this);
  auto It = ChildNodes.insert(std::move(ArgNode));
  ChildIters.push_back(It);
  // We will have to recalculate opcode weights after appending new node
  NodeDistOpt.reset();
}

SmallVector<double> ChoiceNode::getChildWeights() const {
  SmallVector<double> WeightDist;
  WeightDist.reserve(size());
  llvm::transform(ChildIters, std::back_inserter(WeightDist),
                  [](auto &&ChildIter) {
                    assert((*ChildIter).get());
                    auto W = (*ChildIter)->getWeight();
                    return W;
                  });
  return WeightDist;
}

bool ChoiceNode::compareLess(const BaseNode &Rhs) const {
  auto *TruePtrNode = dyn_cast<ChoiceNode>(&Rhs);
  if (!TruePtrNode)
    return getNodeStorageType() < Rhs.getNodeStorageType();
  auto &RhsNodes = TruePtrNode->ChildNodes;
  return std::lexicographical_compare(
      ChildNodes.begin(), ChildNodes.end(), RhsNodes.begin(), RhsNodes.end(),
      [&](auto &&LhsPtr, auto &&RhsPtr) { return *LhsPtr < *RhsPtr; });
}

bool ChoiceNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<ChoiceNode>(&Rhs))
    return compareTwoPtrSequences(ChildNodes, TruePtrNode->ChildNodes);
  return false;
}

BaseNode::NodeHandle ChoiceNode::clone() const {
  SmallVector<BaseNode::NodeHandle> CloneArgs;
  llvm::transform(ChildNodes, std::back_inserter(CloneArgs),
                  [](auto &&ArgPtr) { return ArgPtr->clone(); });
  return BaseNode::create<ChoiceNode>(std::move(CloneArgs));
}

void ChoiceNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

ChoiceNode::ResultSequenceType ChoiceNode::evaluateRandChildNode() const {
  assert(NodeDistOpt.has_value());
  auto RandID = NodeDistOpt.value()(RandEngine::engine());
  assert(ChildIters.size() == ChildNodes.size());
  assert(RandID < ChildIters.size());
  assert(ChildIters[RandID]->get());
  return ChildIters[RandID]->get()->evaluate();
}

ChoiceNode::ResultSequenceType ChoiceNode::evaluate() const {
  if (!NodeDistOpt.has_value()) {
    auto NodeWeights = getChildWeights();
    NodeDistOpt = std::discrete_distribution<size_t>(NodeWeights.begin(),
                                                     NodeWeights.end());
  }
  return evaluateRandChildNode();
}

CartesianNode::CartesianNode(NodeHandle Arg) {
  insert(std::move(Arg));
  setChildsParentLink(ChildNodes, this);
}
CartesianNode::CartesianNode(SmallVector<NodeHandle> Args) {
  for (auto &&ArgNode : Args)
    insert(std::move(ArgNode));
  setChildsParentLink(ChildNodes, this);
}

double CartesianNode::getWeight() const {
  auto WeightRange = llvm::map_range(ChildNodes, [](auto &&NodePtr) {
    assert(NodePtr);
    return NodePtr->getWeight();
  });
  return std::accumulate(WeightRange.begin(), WeightRange.end(), /* init */ 1.0,
                         std::multiplies<double>{});
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

bool CartesianNode::compareLess(const BaseNode &Rhs) const {
  auto *TruePtrNode = dyn_cast<CartesianNode>(&Rhs);
  if (!TruePtrNode)
    return getNodeStorageType() < Rhs.getNodeStorageType();
  auto &RhsNodes = TruePtrNode->ChildNodes;
  return std::lexicographical_compare(
      ChildNodes.begin(), ChildNodes.end(), RhsNodes.begin(), RhsNodes.end(),
      [&](auto &&LhsPtr, auto &&RhsPtr) { return *LhsPtr < *RhsPtr; });
}

bool CartesianNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<CartesianNode>(&Rhs))
    return compareTwoPtrSequences(ChildNodes, TruePtrNode->ChildNodes);
  return false;
}

BaseNode::NodeHandle CartesianNode::clone() const {
  SmallVector<BaseNode::NodeHandle> CloneArgs;
  llvm::transform(ChildNodes, std::back_inserter(CloneArgs),
                  [](auto &&ArgPtr) { return ArgPtr->clone(); });
  return BaseNode::create<CartesianNode>(std::move(CloneArgs));
}

void CartesianNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
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

bool RepeatNode::compareLess(const BaseNode &Rhs) const {
  auto *TruePtrNode = dyn_cast<RepeatNode>(&Rhs);
  if (!TruePtrNode)
    return getNodeStorageType() < Rhs.getNodeStorageType();
  auto LhsTuple = std::make_tuple(degree(), range());
  auto RhsTuple = std::make_tuple(TruePtrNode->degree(), TruePtrNode->range());
  return std::tie(LhsTuple, *getArg()) < std::tie(RhsTuple, *TruePtrNode);
}

bool RepeatNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<RepeatNode>(&Rhs))
    return std::make_tuple(degree(), range()) ==
               std::make_tuple(TruePtrNode->degree(), TruePtrNode->range()) &&
           *getArg() == *TruePtrNode->getArg();
  return false;
}

void RepeatNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

bool HistogramNode::compareLess(const BaseNode &Rhs) const {
  auto *TruePtrNode = dyn_cast<HistogramNode>(&Rhs);
  if (!TruePtrNode)
    return getNodeStorageType() < Rhs.getNodeStorageType();
  auto LhsTuple = std::make_tuple(getName(), getWeight());
  auto RhsTuple =
      std::make_tuple(TruePtrNode->getName(), TruePtrNode->getWeight());
  return std::tie(LhsTuple, getArg()) < std::tie(RhsTuple, *TruePtrNode);
}

bool HistogramNode::isEqual(const BaseNode &Rhs) const {
  if (auto *TruePtrNode = dyn_cast<HistogramNode>(&Rhs))
    return std::make_tuple(getName(), getWeight()) ==
               std::make_tuple(TruePtrNode->getName(),
                               TruePtrNode->getWeight()) &&
           getArg() == *TruePtrNode;
  return false;
}

void HistogramNode::accept(HistogramVisitor &HistVis) const {
  HistVis.visit(*this);
}

} // namespace snippy
} // namespace llvm
