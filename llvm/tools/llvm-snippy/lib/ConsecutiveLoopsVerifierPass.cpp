//===-- ConsecutiveLoopsVerifierPass.cpp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/CFPermutationPass.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "snippy-consecutive-loop-verifier"
#define PASS_DESC "Snippy Consecutive Loops Verifier"

namespace llvm {
namespace snippy {
namespace {

class ConsecutiveLoopsVerifier final : public MachineFunctionPass {
public:
  static char ID;

  ConsecutiveLoopsVerifier() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<CFPermutation>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char ConsecutiveLoopsVerifier::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::ConsecutiveLoopsVerifier;

INITIALIZE_PASS_BEGIN(ConsecutiveLoopsVerifier, DEBUG_TYPE, PASS_DESC, true,
                      true)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_END(ConsecutiveLoopsVerifier, DEBUG_TYPE, PASS_DESC, true, true)

namespace llvm {

MachineFunctionPass *createConsecutiveLoopsVerifierPass() {
  return new ConsecutiveLoopsVerifier();
}

namespace snippy {

bool ConsecutiveLoopsVerifier::runOnMachineFunction(MachineFunction &MF) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &Ctx = GC.getLLVMState().getCtx();
  auto &CLI = getAnalysis<CFPermutation>().get<ConsecutiveLoopInfo>(MF);
  auto &ConsLoops = CLI.getConsecutiveLoops();
  auto &BranchSettings = GC.getGenSettings().Cfg.Branches;
  auto AnyConsLoops = BranchSettings.anyConsecutiveLoops();
  if (!AnyConsLoops) {
    // Check that no consecutive loops were generated if they were not requested
    if (ConsLoops.size() != 0)
      fatal(Ctx, PASS_DESC,
            "No consecutive loops expected to be generated, generated " +
                Twine(ConsLoops.size()));
    return false;
  }

  auto &MLI = getAnalysis<MachineLoopInfo>();
  // Check max depth == 1
  if (any_of(MF, [&MLI](auto &MBB) { return MLI.getLoopDepth(&MBB) > 1; }))
    fatal(Ctx, PASS_DESC,
          "loop depth > 1 is not allowed with consecutive loops");

  if (!BranchSettings.onlyConsecutiveLoops()) {
    // Check that number of loops in pack satisfies requirements
    auto NConsLoops = BranchSettings.getNConsecutiveLoops();
    if (any_of(make_second_range(ConsLoops),
               [NConsLoops](auto &Cons) { return Cons.size() > NConsLoops; }))
      fatal(Ctx, PASS_DESC, "too many consecutive loop were generated");
  } else {
    // Check that all blocks are consectuve loops
    auto NBBs = MF.size();
    if (ConsLoops.size() != 1)
      fatal(Ctx, PASS_DESC, "only consecutive loops chain expected");
    auto It = ConsLoops.find(0u);
    if (It == ConsLoops.end())
      fatal(Ctx, PASS_DESC,
            "the first BB expected to be the first consecutive loop");
    // NBBs == (NConsecutiveLoops + TerminatingBlock + FirstConsecutiveLoop +
    // CommonPreheader)
    if (It->second.size() != NBBs - 3)
      fatal(Ctx, PASS_DESC, "all blocks expeted to be consecutive loops");
  }

  // Check that loops oredred sequentially, i.e. N, N+1, N+2, N+3 ...
  if (any_of(ConsLoops, [](auto &CLEntry) {
        auto BBNums = seq_inclusive(CLEntry.first + 1lu,
                                    CLEntry.first + CLEntry.second.size());
        return !equal(CLEntry.second, BBNums);
      }))
    fatal(Ctx, PASS_DESC, "loops order is not sequential");

  // Check that loops are really generated consecutively
  if (any_of(make_second_range(ConsLoops), [&](auto &Cons) {
        return any_of(Cons, [&](auto &&CLNum) {
          auto *ThisLoopHeader = MF.getBlockNumbered(CLNum);
          auto *PrevLoopHeader = MF.getBlockNumbered(CLNum - 1);
          return !MLI.isLoopHeader(ThisLoopHeader) ||
                 !MLI.isLoopHeader(PrevLoopHeader) ||
                 !MLI.isLoopHeader(ThisLoopHeader->getPrevNode());
        });
      }))
    fatal(Ctx, PASS_DESC, "loops are not consecutive");

  // Check that there is no first loop in consecutive loops set
  if (any_of(make_first_range(ConsLoops), [&ConsLoops](auto FirstLoop) {
        return any_of(make_second_range(ConsLoops), [FirstLoop](auto &Cons) {
          return Cons.count(FirstLoop);
        });
      }))
    fatal(Ctx, PASS_DESC,
          "first consecutive loop found in other cosecutive loops set");

  return false;
}

} // namespace snippy
} // namespace llvm
