//===-- ConsecutiveLoopsVerifierPass.cpp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Support/Options.h"

#include "llvm/InitializePasses.h"

#define DEBUG_TYPE "snippy-consecutive-loop-verifier"
#define PASS_DESC "Snippy Consecutive Loops Verifier"

namespace llvm {
namespace snippy {
namespace {

class ConsecutiveLoopsVerifier final : public MachineFunctionPass {
public:
  static char ID;

  ConsecutiveLoopsVerifier();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
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

ConsecutiveLoopsVerifier::ConsecutiveLoopsVerifier() : MachineFunctionPass(ID) {
  initializeConsecutiveLoopsVerifierPass(*PassRegistry::getPassRegistry());
}

bool ConsecutiveLoopsVerifier::runOnMachineFunction(MachineFunction &MF) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &Ctx = GC.getLLVMState().getCtx();
  auto &ConsLoops = GC.getConsecutiveLoops();
  auto NConsLoops = GC.getGenSettings().Cfg.Branches.NConsecutiveLoops;
  if (NConsLoops == 0) {
    // Check that no consecutive loops were generated if they were not requested
    if (ConsLoops.size() != 0)
      fatal(Ctx, "CFG verification",
            "No consecutive loops expected to be generated, generated " +
                Twine(ConsLoops.size()));
    return false;
  }

  auto &MLI = getAnalysis<MachineLoopInfo>();
  // Check max depth == 1
  if (any_of(MF, [&MLI](auto &MBB) { return MLI.getLoopDepth(&MBB) > 1; }))
    fatal(Ctx, "CFG verification",
          "loop depth > 1 is not allowed with consecutive loops");

  // Check that number of loops in pack satisfies requirements
  if (any_of(make_second_range(ConsLoops),
             [NConsLoops](auto &Cons) { return Cons.size() > NConsLoops; }))
    fatal(Ctx, "CFG verification", "too many consecutive loop were generated");

  // Check that loops oredred sequentially, i.e. N, N+1, N+2, N+3 ...
  if (any_of(ConsLoops, [](auto &CLEntry) {
        auto BBNums = seq_inclusive(CLEntry.first + 1lu,
                                    CLEntry.first + CLEntry.second.size());
        return !equal(CLEntry.second, BBNums);
      }))
    fatal(Ctx, "CFG verification", "loops order is not sequential");

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
    fatal(Ctx, "CFG verification", "loops are not consecutive");

  // Check that there is no first loop in consecutive loops set
  if (any_of(make_first_range(ConsLoops), [&ConsLoops](auto FirstLoop) {
        return any_of(make_second_range(ConsLoops), [FirstLoop](auto &Cons) {
          return Cons.count(FirstLoop);
        });
      }))
    fatal(Ctx, "CFG verification",
          "first consecutive loop found in other cosecutive loops set");

  return false;
}

} // namespace snippy
} // namespace llvm
