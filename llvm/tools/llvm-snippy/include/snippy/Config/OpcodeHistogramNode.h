//===-- OpcodeHistogramNode.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMNODE_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMNODE_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include "snippy/Support/RandUtil.h"

#include <optional>
#include <random>

namespace llvm {
namespace snippy {

class BaseNode;
class HistogramVisitor;

namespace detail {

template <typename NodeType, typename... Args>
static constexpr bool IsConstructibleNode =
    std::is_base_of_v<BaseNode, NodeType> &&
    std::is_constructible_v<NodeType, Args...>;

} // namespace detail

class BaseNode {
public:
  enum class NodeStorageType {
    OpcodeNode,
    NumberNode,
    CompositeNode,
    ChoiceNode,
    CartesianNode,
    RepeatNode,
    HistogramNode
  };

  static constexpr double DefaultNodeWeight = 1.0;

  using NodeHandle = std::unique_ptr<BaseNode>;
  using ResultSequenceType = std::vector<unsigned>;

  BaseNode(BaseNode *Parent = nullptr) : Parent(Parent) {}
  virtual ~BaseNode() = default;

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  static std::unique_ptr<NodeTy> create(ArgsTy &&...Values) {
    return std::make_unique<NodeTy>(std::forward<ArgsTy>(Values)...);
  }

  void setParent(const BaseNode *ParentPtr) { Parent = ParentPtr; }
  const BaseNode *getParent() const { return Parent; }

  virtual double getWeight() const { return DefaultNodeWeight; }
  virtual ResultSequenceType evaluate() const = 0;
  virtual void accept(HistogramVisitor &) const = 0;
  virtual NodeHandle clone() const = 0;
  virtual NodeStorageType getNodeStorageType() const = 0;

protected:
  const BaseNode *Parent;
};

class NumberNode : public BaseNode {
public:
  static constexpr unsigned ActiveWordBits = sizeof(unsigned) * CHAR_BIT;

  NumberNode(APInt Num) : Num(Num) {}
  NumberNode(unsigned Num, bool IsSigned = false)
      : Num(ActiveWordBits, Num, IsSigned) {}
  NumberNode(StringRef StrNum, uint8_t Radix = 10)
      : Num(ActiveWordBits, StrNum, Radix) {}

  unsigned getNum() const {
    assert(Num.getActiveBits() <= ActiveWordBits &&
           "Too many bits for unsigned");
    assert(isNonNegative() && "Non-negative number is expected");
    return Num.getZExtValue();
  }

  NodeHandle clone() const override {
    return std::make_unique<NumberNode>(Num);
  }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::NumberNode;
  }

  ResultSequenceType evaluate() const override {
    assert(isNonNegative());
    return {static_cast<unsigned>(Num.getZExtValue())};
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::NumberNode;
  }

  void accept(HistogramVisitor &HistVis) const override;

  bool isNonPositive() const { return Num.isNonPositive(); }
  bool isNonNegative() const { return Num.isNonNegative(); }

protected:
  APInt Num;
};

// Node storing an opcode and its weight
class OpcodeNode : public NumberNode {
public:
  // OpcodeHistogram can contain top opcodes using the syntax:
  //   - [Opcode, Weight]
  // It can also contain patterns in the form of opcodes, e.g:
  //   AddPattern: "ADD"
  //   histogram:
  //     - [Opcode, Weight] # Top opcode
  //     - [pattern: AddPattern, Weight] # Opcode pattern
  enum class OpcodeCategory { Top, Pattern };

  OpcodeNode(unsigned Num, double Weight = DefaultNodeWeight,
             OpcodeCategory Categ = OpcodeCategory::Pattern)
      : NumberNode(Num), Weight(Weight), Category(Categ) {}

  double getWeight() const override { return Weight; }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::OpcodeNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::OpcodeNode;
  }

  NodeHandle clone() const override {
    return std::make_unique<OpcodeNode>(Num.getZExtValue(), Weight, Category);
  }

  bool isTopOpcode() const { return Category == OpcodeCategory::Top; }

  void accept(HistogramVisitor &HistVis) const override;

private:
  double Weight;
  OpcodeCategory Category;
};

// Wrapper for a histogram pattern
class HistogramNode : public BaseNode {
public:
  HistogramNode(StringRef Name, NodeHandle HistNodePtr,
                double Weight = DefaultNodeWeight)
      : Name(Name), HistNode(std::move(HistNodePtr)), Weight(Weight) {
    assert(HistNode);
    HistNode->setParent(this);
  }

  ResultSequenceType evaluate() const override {
    assert(HistNode);
    return HistNode->evaluate();
  }

  NodeHandle clone() const override {
    return std::make_unique<HistogramNode>(Name, HistNode->clone(), Weight);
  }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::HistogramNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::HistogramNode;
  }

  std::string getName() const { return Name; }

  double getWeight() const override { return Weight; }

  void accept(HistogramVisitor &HistVis) const override;

  BaseNode &getArg() const {
    assert(HistNode);
    return *HistNode;
  }

private:
  std::string Name;
  NodeHandle HistNode;
  double Weight;
};

namespace detail {
// Base pure virtual class for working with child nodes
class CompositeNode {
public:
  virtual ~CompositeNode() = default;

  virtual void insert(BaseNode::NodeHandle ArgNode) = 0;

  unsigned size() const { return ChildNodes.size(); }
  bool empty() const { return ChildNodes.empty(); }

  auto begin() { return ChildNodes.begin(); }
  auto end() { return ChildNodes.end(); }
  auto begin() const { return ChildNodes.begin(); }
  auto end() const { return ChildNodes.end(); }

  SmallVector<double> getChildWeights() const;

  double getTotalChildsWeight() const;

protected:
  CompositeNode() = default;
  CompositeNode(BaseNode::NodeHandle Arg) {
    assert(Arg);
    ChildNodes.push_back(std::move(Arg));
  }
  CompositeNode(const CompositeNode &Rhs) = delete;
  CompositeNode(CompositeNode &&Rhs) = default;
  CompositeNode &operator=(const CompositeNode &Rhs) = delete;
  CompositeNode &operator=(CompositeNode &&Rhs) = default;

  CompositeNode(SmallVector<BaseNode::NodeHandle> ArgNodes)
      : ChildNodes(std::move(ArgNodes)) {}

  void setChildsParentLink(const BaseNode *Parent) {
    llvm::for_each(ChildNodes, [Parent](auto &&ChildPtr) {
      assert(ChildPtr);
      ChildPtr->setParent(Parent);
    });
  }

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  void emplaceImpl(const BaseNode *Parent, ArgsTy &&...Values) {
    auto NewNode = std::make_unique<NodeTy>(std::forward<ArgsTy>(Values)...);
    NewNode->setParent(Parent);
    insert(std::move(NewNode));
  }

  template <typename CompositeNodeTy> BaseNode::NodeHandle cloneImpl() const {
    SmallVector<BaseNode::NodeHandle> CloneArgs;
    llvm::transform(ChildNodes, std::back_inserter(CloneArgs),
                    [](auto &&ArgPtr) { return ArgPtr->clone(); });
    return std::make_unique<CompositeNodeTy>(std::move(CloneArgs));
  }

  SmallVector<BaseNode::NodeHandle> ChildNodes;
};

} // namespace detail

// The main class that defines the tree of opcodes/patterns. It's used to get a
// random opcode/pattern according to its corresponding weight. E.g:
//                  ------------
//           ------ |ChoiceNode| ----------
//           |      ------------          |
//           |             |              |
//       [ADD, 1.0]    [SUB, 1.0]   [pattern, 2.0]
// Gives either ADD (probability 1 / 4), SUB (probability 1 / 4), or a pattern
// (probability 1 / 2)
class ChoiceNode : public detail::CompositeNode, public BaseNode {
public:
  using OpcodeDistType = std::discrete_distribution<size_t>;

  ChoiceNode() = default;
  ChoiceNode(NodeHandle Arg) : CompositeNode(std::move(Arg)) {
    setChildsParentLink(this);
  }
  ChoiceNode(SmallVector<NodeHandle> Args) : CompositeNode(std::move(Args)) {
    setChildsParentLink(this);
  }
  ChoiceNode(const ChoiceNode &Rhs);
  ChoiceNode(ChoiceNode &&Rhs);
  ChoiceNode &operator=(const ChoiceNode &Rhs);
  ChoiceNode &operator=(ChoiceNode &&Rhs);

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  void emplace(ArgsTy &&...Values) {
    emplaceImpl<NodeTy>(this, std::forward<ArgsTy>(Values)...);
    // We will have to recalculate opcode weights after appending new node
    NodeDistOpt.reset();
  }

  void insert(NodeHandle ArgNode) override {
    assert(ArgNode);
    ArgNode->setParent(this);
    ChildNodes.push_back(std::move(ArgNode));
    // We will have to recalculate opcode weights after appending new node
    NodeDistOpt.reset();
  }

  ResultSequenceType evaluate() const override;

  NodeHandle clone() const override { return cloneImpl<ChoiceNode>(); }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::ChoiceNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::ChoiceNode;
  }

  void accept(HistogramVisitor &HistVis) const override;

private:
  ResultSequenceType evaluateRandChildNode() const;

  mutable std::optional<OpcodeDistType> NodeDistOpt;
};

// A Node storing the Cartesian product of its children. Similar to ChoiceNode,
// it gives a random sequence of opcodes. E.g:
// Sequence1: (ADD | MUL)
// Sequence2: (SUB | DIV)
// As a result of Sequence1 * Sequence2, we can get the following sequences:
//   (ADD SUB), (ADD DIV), (MUL SUB), (MUL DIV).
class CartesianNode : public detail::CompositeNode, public BaseNode {
public:
  CartesianNode() = default;
  CartesianNode(NodeHandle Arg) : CompositeNode(std::move(Arg)) {
    setChildsParentLink(this);
  }
  CartesianNode(SmallVector<NodeHandle> Args) : CompositeNode(std::move(Args)) {
    setChildsParentLink(this);
  }

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  void emplace(ArgsTy &&...Values) {
    emplaceImpl<NodeTy>(this, std::forward<ArgsTy>(Values)...);
  }

  void insert(NodeHandle ArgNode) override {
    assert(ArgNode);
    ArgNode->setParent(this);
    ChildNodes.push_back(std::move(ArgNode));
  }

  double getWeight() const override;

  ResultSequenceType evaluate() const override;

  NodeHandle clone() const override { return cloneImpl<CartesianNode>(); }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::CartesianNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::CartesianNode;
  }

  void accept(HistogramVisitor &HistVis) const override;
};

// Duplicates the stored pattern a random number of times, chosen from the
// specified range. E.g:
// In config: "ADD ^ [3 : 10]"
// ArgNode: ADD
// Degree: 5 (randomly chosen from the range [3 : 10])
// Result: (ADD ADD ADD ADD ADD)
class RepeatNode : public BaseNode {
public:
  using RangeType = std::pair<unsigned, unsigned>;

  RepeatNode(NodeHandle NodePtr, const RangeType &Range)
      : ArgNode(std::move(NodePtr)), Range(Range), Degree(Range.first) {
    assert(ArgNode);
    ArgNode->setParent(this);
  }

  RepeatNode(NodeHandle NodePtr, unsigned SingleElem)
      : RepeatNode(std::move(NodePtr), RangeType(SingleElem, SingleElem)) {}

  ResultSequenceType evaluate() const override;

  NodeHandle clone() const override {
    return std::make_unique<RepeatNode>(ArgNode->clone(), Range);
  }

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::RepeatNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::RepeatNode;
  }

  void accept(HistogramVisitor &HistVis) const override;

  const BaseNode *getArg() const { return ArgNode.get(); }

  RangeType range() const { return Range; }

  unsigned degree() const { return Degree; }

private:
  unsigned generateDegree() const {
    return RandEngine::genInRangeInclusive<unsigned>(Range.first, Range.second);
  }

  NodeHandle ArgNode;
  RangeType Range;
  // Last generated degree
  mutable unsigned Degree;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_OPCODEHISTOGRAMNODE_H
