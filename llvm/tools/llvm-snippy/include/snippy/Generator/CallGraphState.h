//===-- CallGraphState.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This file defines CallGraphState class which is used to construct and
/// analyze snippet call graph.
///
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/Debug.h"
#include <queue>

namespace llvm {
namespace snippy {

enum class CallGraphDumpMode { Dot, Yaml };

class CallGraphState final {
public:
  class Node;

  struct Edge {
    Node *Dest;
    bool Committed;

    Edge(Node *Dest = nullptr, bool Committed = false)
        : Dest(Dest), Committed(Committed){};

    // Needed by GraphTraits.
    operator Node *() const { return Dest; }
  };

  class Node {
  public:
    /*
     * class Node.
     *
     * Represents a node of directed call graph.
     * It has two types of edges: callee and caller.
     *
     * If Node #1 has callee edge to Node #2,
     * then Node #2 has caller edge to Node #1
     * This invariant is kept by this class.
     *
     * One node unites a group of one or more functions
     * sharing same callees and callers.
     *
     */

    explicit Node(const Function *Func) : Fs(1u, Func){};

    // Creates caller(this) <-> callee(N) relationship.
    void addCallee(Node *N);

    // Removes caller(this) <-> callee(N) relationship.
    void removeCallee(Node *N);

    // Marks caller(this) <-> callee(N) relationship as commited.
    void markAsCommitted(Node *N);

    // Erases all callee edges.
    void removeAllCallees();

    // Erases all caller edges.
    void removeFromAllCallers();

    // Erases all edges.
    void extract();

    auto &callees() const { return Callees; }
    auto &callers() const { return Callers; }

    bool hasCallee(const Node *Callee) const;
    bool hasCaller(const Node *Caller) const;

    void append(const Function *F) { Fs.push_back(F); }
    auto &functions() const { return Fs; }

    using ChildContainerT = std::vector<Edge>;
    using ChildIteratorT = ChildContainerT::const_iterator;

  private:
    SmallVector<const Function *, 2> Fs;
    ChildContainerT Callees;
    ChildContainerT Callers;
  };

  using NodeContainerT = SmallVector<std::unique_ptr<Node>, 5>;
  using NodeRefContainerT = SmallVector<Node *, 5>;

private:
  NodeContainerT Nodes;
  NodeRefContainerT NodeRefs;
  DenseMap<const Function *, Node *> FunToNodeMap;

public:
  CallGraphState() = default;

  bool registered(const Function *F) const { return FunToNodeMap.count(F); }
  auto *emplaceNode(const Function *F) {
    assert(!registered(F) && "Node for MF has been already registered");
    auto *N = Nodes.emplace_back(std::make_unique<Node>(F)).get();
    NodeRefs.emplace_back(N);
    FunToNodeMap.insert({F, N});
    return N;
  }

  auto *getNode(const Function *F) const {
    assert(registered(F) && "Machine Function is unregistered");
    return FunToNodeMap.lookup(F);
  }

  void appendNode(Node *N, const Function *F) {
    FunToNodeMap.insert({F, N});
    N->append(F);
  }

  // Checks if Node of F is reachable from root node via callee edges.
  // If CheckCommit is enabled, edge that is not committed are skipped.
  bool reachable(const Function *F, bool CheckCommit = true) const;

  auto nodes_count() const { return NodeRefs.size(); }
  auto nodes_begin() const { return NodeRefs.begin(); }
  auto nodes_end() const { return NodeRefs.end(); }

  auto *getRootNode() const { return NodeRefs.front(); }

  bool isRoot(const Function *F) const {
    return registered(F) && getNode(F) == getRootNode();
  }

  void setRoot(Node *Node) {
    if (Node == getRootNode())
      return;
    auto NodeIt = std::find_if(NodeRefs.begin(), NodeRefs.end(),
                               [Node](auto &N) { return N == Node; });
    assert(NodeIt != NodeRefs.end() && "Node is unregistered");
    std::swap(*NodeIt, NodeRefs.front());
  }

  void dump(StringRef Filename, CallGraphDumpMode Format) const;
};

} // namespace snippy

template <> struct GraphTraits<snippy::CallGraphState> {
  using NodeRef = snippy::CallGraphState::Node *;
  using ChildIteratorType = snippy::CallGraphState::Node::ChildIteratorT;

  static NodeRef getEntryNode(const snippy::CallGraphState &CGS) {
    return CGS.getRootNode();
  }

  static ChildIteratorType child_begin(NodeRef Node) {
    return Node->callees().begin();
  }

  static ChildIteratorType child_end(NodeRef Node) {
    return Node->callees().end();
  }

  using nodes_iterator =
      snippy::CallGraphState::NodeRefContainerT::const_iterator;

  static nodes_iterator nodes_begin(const snippy::CallGraphState &G) {
    return G.nodes_begin();
  }
  static nodes_iterator nodes_end(const snippy::CallGraphState &G) {
    return G.nodes_end();
  }

  using EdgeRef = snippy::CallGraphState::Edge;

  using ChildEdgeIteratorType = ChildIteratorType;
  static ChildEdgeIteratorType child_edge_begin(NodeRef Node) {
    return child_begin(Node);
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef Node) {
    return child_end(Node);
  }

  static NodeRef edge_dest(EdgeRef Edge) { return Edge; }

  static unsigned size(const snippy::CallGraphState &G) {
    return G.nodes_count();
  }
};

template <>
struct DOTGraphTraits<snippy::CallGraphState> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool simple = false) : DefaultDOTGraphTraits(simple) {}

  static std::string getNodeLabel(const snippy::CallGraphState::Node *N,
                                  const snippy::CallGraphState &);

  static std::string getNodeAttributes(const snippy::CallGraphState::Node *N,
                                       const snippy::CallGraphState &CGS);

  static std::string
  getEdgeAttributes(const snippy::CallGraphState::Node *N,
                    snippy::CallGraphState::Node::ChildIteratorT EdgeIt,
                    const snippy::CallGraphState &);
};

} // namespace llvm
