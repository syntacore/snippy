//===-- CFGPrinter.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef CFGPRINTER_H
#define CFGPRINTER_H

#include "snippy/Support/Utils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/DOTGraphTraits.h"

namespace llvm {

class MachineFunction;

template <>
struct DOTGraphTraits<const MachineFunction *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool IsSimple = false) : DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const MachineFunction *F) {
    return ("CFG for '" + F->getName() + "' function").str();
  }

  std::string getNodeLabel(const MachineBasicBlock *Node,
                           const MachineFunction *Graph);
};

namespace snippy {

bool shouldDumpCFGBeforePass(StringRef PassID);
bool shouldDumpCFGAfterPass(StringRef PassID);

bool shouldViewCFGBeforePass(StringRef PassID);
bool shouldViewCFGAfterPass(StringRef PassID);

} // namespace snippy
} // namespace llvm

#endif
