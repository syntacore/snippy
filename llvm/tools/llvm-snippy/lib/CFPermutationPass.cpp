//===-- CFPermutationPass.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Control Flow Permutation pass implementation. This pass takes machine
/// function with only blocks and fallback branches and permutes them saving
/// structured control flow (CF).
///
/// Algorithm in general:
///   1. Take random BB for permutation (branch source);
///   2. Take random BB from available set for selected BB (branch destination);
///   3. Update available sets and other service info;
///   4. Return to 1. until all branches are permuted.
///
/// Available set is a support data structure that helps to track candidates for
/// branch destination and save structured CF.
///
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/Generator/CFPermutation.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "snippy-cf-permutation"
#define PASS_DESC "Snippy Control Flow Permutation"

namespace llvm {
namespace snippy {
namespace {
class CFPermutation final : public MachineFunctionPass {
public:
  static char ID;

  CFPermutation() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<FunctionGenerator>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char CFPermutation::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CFPermutation;

INITIALIZE_PASS_BEGIN(CFPermutation, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(CFPermutation, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createCFPermutationPass() { return new CFPermutation(); }

namespace snippy {
bool CFPermutation::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(
      dbgs() << "CFPermutation::runOnMachineFunction runs on function:\n");
  LLVM_DEBUG(MF.dump());
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &FG = getAnalysis<FunctionGenerator>();
  return CFPermutationContext(MF, GC, FG).makePermutationAndUpdateBranches();
}
} // namespace snippy
} // namespace llvm
