//===-- CallGraphState.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/CallGraphState.h"
#include "snippy/Support/Utils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/YAMLTraits.h"

#include <sstream>
#include <stack>

namespace llvm {
namespace snippy {

bool CallGraphState::Node::hasCallee(const Node *Callee) const {
  return std::any_of(Callees.begin(), Callees.end(),
                     [Callee](auto &C) { return C == Callee; });
}

bool CallGraphState::Node::hasCaller(const Node *Caller) const {
  return std::any_of(Callers.begin(), Callers.end(),
                     [Caller](auto &C) { return C == Caller; });
}

void CallGraphState::Node::addCallee(Node *N) {
  auto Found = std::find_if(Callees.begin(), Callees.end(),
                            [N](auto &C) { return C == N; });
  assert(Found == Callees.end() && "Double insertion");
  Callees.emplace_back(N, false);
  N->Callers.emplace_back(this, false);
}

void CallGraphState::Node::removeCallee(Node *N) {
  auto Found = std::find_if(Callees.begin(), Callees.end(),
                            [N](auto &C) { return C == N; });
  assert(Found != Callees.end() && "Unregistered callee");
  auto &Callers = Found->Dest->Callers;
  Callers.erase(
      std::remove_if(Callers.begin(), Callers.end(),
                     [this](auto &Caller) { return Caller.Dest == this; }),
      Callers.end());
}

void CallGraphState::Node::markAsCommitted(Node *N) {
  auto Found = std::find_if(Callees.begin(), Callees.end(),
                            [N](auto &C) { return C == N; });
  assert(Found != Callees.end() && "Unregistered callee");
  Found->Committed = true;
}

void CallGraphState::Node::removeAllCallees() {
  while (!Callees.empty()) {
    removeCallee(Callees.front());
  }
}

void CallGraphState::Node::removeFromAllCallers() {
  while (!Callers.empty()) {
    Callers.back().Dest->removeCallee(this);
  }
}

void CallGraphState::Node::extract() {
  removeFromAllCallers();
  removeAllCallees();
}

bool CallGraphState::reachable(const Function *F, bool CheckCommit) const {
  auto *N = getNode(F);

  std::stack<Node *> TraverseStack;
  DenseSet<Node *> Visited;

  auto *Root = getRootNode();
  TraverseStack.push(getRootNode());
  Visited.insert(Root);
  while (!TraverseStack.empty()) {
    auto CurNode = TraverseStack.top();
    TraverseStack.pop();
    if (N == CurNode)
      return true;
    for (auto &Callee : CurNode->callees())
      if (!Visited.contains(Callee) && (Callee.Committed || !CheckCommit)) {
        TraverseStack.push(Callee);
        Visited.insert(Callee);
      }
  }

  return false;
}

void CallGraphState::dump(StringRef Filename, CallGraphDumpMode Format) const {

  if (Format == CallGraphDumpMode::Dot) {
    auto WriteResult = WriteGraph(*this, "", /* ShortNames */ false,
                                  "Call graph", std::string(Filename));
    if (WriteResult.empty())
      report_fatal_error("Failed to write call graph to " + Twine(Filename),
                         false);

    return;
  }

  if (Nodes.empty())
    report_fatal_error("Cannot dump empty call graph.", false);

  auto NameOf = [](Node *N) { return N->functions().front()->getName().str(); };

  std::stringstream SS;
  SS << "entry-point: " << NameOf(getRootNode()) << "\n";
  SS << "function-list:"
     << "\n";
  for (auto &N : NodeRefs) {
    SS << "  - name: " << NameOf(N) << "\n";
    // Snippy emits 'external' functions with weak linkage.
    // Generated functions have either external or internal one.
    if (Function::isWeakLinkage(N->functions().front()->getLinkage()))
      SS << "    external: true\n";
    if (N->callees().empty())
      continue;
    SS << "    callees:\n";
    for (auto &E : N->callees()) {
      SS << "      - " << NameOf(E.Dest) << "\n";
    }
  }

  writeFile(Filename, SS.str());
}

} // namespace snippy

std::string DOTGraphTraits<snippy::CallGraphState>::getNodeLabel(
    const snippy::CallGraphState::Node *N, const snippy::CallGraphState &) {
  return std::string(N->functions().front()->getName());
}

std::string DOTGraphTraits<snippy::CallGraphState>::getNodeAttributes(
    const snippy::CallGraphState::Node *N, const snippy::CallGraphState &CGS) {
  std::string Attrs = "style=filled";
  // Reachability is expressed as color.
  if (CGS.getRootNode() == N)
    Attrs += " fillcolor=lightskyblue";
  else if (CGS.reachable(N->functions().front()))
    Attrs += " fillcolor=green";
  else
    Attrs += " fillcolor=grey";

  // Snippy-generated functions are shown as boxes
  // and external are shown as  diamonds.
  auto *F = N->functions().front();
  if (!Function::isWeakLinkage(F->getLinkage())) {
    Attrs += " shape=record";
  } else {
    Attrs += " shape=diamond";
  }

  return Attrs;
}

std::string DOTGraphTraits<snippy::CallGraphState>::getEdgeAttributes(
    const snippy::CallGraphState::Node *N,
    snippy::CallGraphState::Node::ChildIteratorT EdgeIt,
    const snippy::CallGraphState &) {
  auto Committed = EdgeIt->Committed;
  if (Committed)
    return "";
  else
    return "style=dashed color=grey";
}

} // namespace llvm
