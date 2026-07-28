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
  virtual bool compareLess(const BaseNode &Rhs) const = 0;
  virtual bool isEqual(const BaseNode &Rhs) const = 0;

  friend bool operator<(const BaseNode &Lhs, const BaseNode &Rhs) {
    return Lhs.compareLess(Rhs);
  }

  friend bool operator==(const BaseNode &Lhs, const BaseNode &Rhs) {
    return Lhs.isEqual(Rhs);
  }

  friend bool operator!=(const BaseNode &Lhs, const BaseNode &Rhs) {
    return !Lhs.isEqual(Rhs);
  }

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
    return BaseNode::create<NumberNode>(Num);
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

  bool compareLess(const BaseNode &Rhs) const override;
  bool isEqual(const BaseNode &Rhs) const override;
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

  bool compareLess(const BaseNode &Rhs) const override;

  bool isEqual(const BaseNode &Rhs) const override;

  NodeHandle clone() const override {
    return BaseNode::create<OpcodeNode>(Num.getZExtValue(), Weight, Category);
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

  bool compareLess(const BaseNode &Rhs) const override;

  bool isEqual(const BaseNode &Rhs) const override;

  NodeHandle clone() const override {
    return BaseNode::create<HistogramNode>(Name, HistNode->clone(), Weight);
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

// The main class that defines the tree of opcodes/patterns. It's used to get a
// random opcode/pattern according to its corresponding weight. E.g:
//                  ------------
//           ------ |ChoiceNode| ----------
//           |      ------------          |
//           |             |              |
//       [ADD, 1.0]    [SUB, 1.0]   [pattern, 2.0]
// Gives either ADD (probability 1 / 4), SUB (probability 1 / 4), or a pattern
// (probability 1 / 2)
class ChoiceNode : public BaseNode {
  struct NodeComparator final {
    bool operator()(const BaseNode::NodeHandle &Lhs,
                    const BaseNode::NodeHandle &Rhs) const {
      assert(Lhs);
      assert(Rhs);
      return *Lhs < *Rhs;
    }
  };

  using StorageType = std::multiset<BaseNode::NodeHandle, NodeComparator>;
  using StorageIteratorTy = StorageType::iterator;

public:
  using OpcodeDistType = std::discrete_distribution<size_t>;

  ChoiceNode() = default;
  ChoiceNode(NodeHandle Arg);
  ChoiceNode(SmallVector<NodeHandle> Args);
  ChoiceNode(const ChoiceNode &Rhs);
  ChoiceNode(ChoiceNode &&Rhs);
  ChoiceNode &operator=(const ChoiceNode &Rhs);
  ChoiceNode &operator=(ChoiceNode &&Rhs);

  void insert(NodeHandle ArgNode);

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  void emplace(ArgsTy &&...Values) {
    auto EmplaceIt = ChildNodes.emplace(
        BaseNode::create<NodeTy>(std::forward<ArgsTy>(Values)...));
    (*EmplaceIt)->setParent(this);
    ChildIters.push_back(EmplaceIt);
    // We will have to recalculate opcode weights after appending new node
    NodeDistOpt.reset();
  }

  template <typename Predicate> void erase(Predicate &&Pred) {
    for (auto Begin = ChildNodes.begin(); Begin != ChildNodes.end();) {
      if (Pred(Begin->get())) {
        // Don't forget to remove the iterator from the storage
        llvm::erase(ChildIters, Begin);
        Begin = ChildNodes.erase(Begin);
        // We will have to recalculate opcode weights after erasing a node
        NodeDistOpt.reset();
      } else {
        ++Begin;
      }
    }
  }

  double getTotalChildsWeight() const {
    auto Weights = getChildWeights();
    return std::accumulate(Weights.begin(), Weights.end(), /* init */ 0.0);
  }

  ResultSequenceType evaluate() const override;

  bool compareLess(const BaseNode &Rhs) const override;

  bool isEqual(const BaseNode &Rhs) const override;

  NodeHandle clone() const override;

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::ChoiceNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::ChoiceNode;
  }

  void accept(HistogramVisitor &HistVis) const override;

  unsigned size() const { return ChildNodes.size(); }
  bool empty() const { return ChildNodes.empty(); }

  auto range() {
    return llvm::make_range(ChildNodes.begin(), ChildNodes.end());
  }
  auto range() const {
    return llvm::make_range(ChildNodes.begin(), ChildNodes.end());
  }

  auto begin() { return ChildNodes.begin(); }
  auto end() { return ChildNodes.end(); }
  auto begin() const { return ChildNodes.begin(); }
  auto end() const { return ChildNodes.end(); }

private:
  SmallVector<double> getChildWeights() const;

  ResultSequenceType evaluateRandChildNode() const;

  // We keep child BaseNodes sorted to enable efficient equality comparison.
  // For example, consider two ChoiceNodes that have the same children but in
  // different order:
  //
  //   ChoiceNode1:        ChoiceNode2:
  //    |       |           |       |
  //   ADD     SUB         SUB     ADD
  //
  // If we stored opcodes in the order they appear, we would have sequences
  // <ADD, SUB> and <SUB, ADD> – they are not lexicographically equal even
  // though the sets of children are identical. By storing them in a
  // std::multiset with a custom comparator we guarantee a canonical order.
  StorageType ChildNodes;
  // For fast random access during evaluation.
  SmallVector<StorageIteratorTy> ChildIters;
  mutable std::optional<OpcodeDistType> NodeDistOpt;
};

// A Node storing the Cartesian product of its children. Similar to ChoiceNode,
// it gives a random sequence of opcodes. E.g:
// Sequence1: (ADD | MUL)
// Sequence2: (SUB | DIV)
// As a result of Sequence1 * Sequence2, we can get the following sequences:
//   (ADD SUB), (ADD DIV), (MUL SUB), (MUL DIV).
class CartesianNode : public BaseNode {
public:
  CartesianNode() = default;
  CartesianNode(NodeHandle Arg);
  CartesianNode(SmallVector<NodeHandle> Args);

  template <typename NodeTy, typename... ArgsTy,
            typename = std::enable_if_t<
                detail::IsConstructibleNode<NodeTy, ArgsTy...>>>
  void emplace(ArgsTy &&...Values) {
    ChildNodes.emplace_back(
        BaseNode::create<NodeTy>(std::forward<ArgsTy>(Values)...));
    ChildNodes.back()->setParent(this);
  }

  void insert(NodeHandle ArgNode) {
    assert(ArgNode);
    ArgNode->setParent(this);
    ChildNodes.push_back(std::move(ArgNode));
  }

  double getWeight() const override;

  ResultSequenceType evaluate() const override;

  bool compareLess(const BaseNode &Rhs) const override;

  bool isEqual(const BaseNode &Rhs) const override;

  NodeHandle clone() const override;

  static bool classof(const BaseNode *BNode) {
    assert(BNode);
    return BNode->getNodeStorageType() == NodeStorageType::CartesianNode;
  }

  NodeStorageType getNodeStorageType() const override {
    return NodeStorageType::CartesianNode;
  }

  void accept(HistogramVisitor &HistVis) const override;

  unsigned size() const { return ChildNodes.size(); }
  bool empty() const { return ChildNodes.empty(); }

  auto begin() { return ChildNodes.begin(); }
  auto end() { return ChildNodes.end(); }
  auto begin() const { return ChildNodes.begin(); }
  auto end() const { return ChildNodes.end(); }

private:
  // We keep an ordered sequence here
  SmallVector<NodeHandle> ChildNodes;
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

  bool compareLess(const BaseNode &Rhs) const override;

  bool isEqual(const BaseNode &Rhs) const override;

  NodeHandle clone() const override {
    return BaseNode::create<RepeatNode>(ArgNode->clone(), Range);
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
