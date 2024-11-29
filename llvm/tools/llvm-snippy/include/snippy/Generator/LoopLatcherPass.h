//===-- LoopLatcherPass.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/ActiveImmutablePass.h"

#include "llvm/CodeGen/MachineLoopInfo.h"

namespace llvm {
namespace snippy {

class SnippyLoopInfo {
public:
  using RegToValueType = DenseMap<Register, APInt>;
  struct LoopGenerationInfo {
    Register CounterReg;
    unsigned NumIter;
    unsigned SmallestCounterVal;
    LoopType Type;
  };

  SnippyLoopInfo() = default;
  void addLoopGenerationInfoForMBB(const MachineBasicBlock *Header,
                                   LoopGenerationInfo LGI) {
    assert(Header);
    [[maybe_unused]] bool Inserted = LoopInfoMap.insert({Header, LGI}).second;
    assert(Inserted);
  }

  std::optional<LoopGenerationInfo>
  getLoopsGenerationInfoForMBB(const MachineBasicBlock *Header) const {
    assert(Header);
    auto Found = LoopInfoMap.find(Header);
    return Found == LoopInfoMap.end() ? std::nullopt
                                      : std::optional(Found->second);
  }

  // See description for `IncomingValues`.
  void addIncomingValues(const MachineBasicBlock *MBB,
                         RegToValueType RegToValue);
  const RegToValueType &getIncomingValues(const MachineBasicBlock *MBB) const;

private:
  DenseMap<const MachineBasicBlock *, LoopGenerationInfo> LoopInfoMap;

  // A map that keeps SOME registers to value pairs for SOME blocks. This is
  // needed only in selfcheck mode and only for blocks which can be entered in
  // an order that doesn't match execution order.
  // Let's consider an example: we're generating a loop in selfcheck mode. Loops
  // in this mode have a special structure that is
  //        ...
  //         |
  //  +------v-----+
  //  |            |
  //  |  preheader |
  //  |            |
  //  +------+-----+
  //         |
  //         | --------------------------
  //         |/                          \
  //  +------v-----+                     |
  //  |            |                     |
  //  |   header   |                     |
  //  |            |                     |
  //  +------+-----+                     |
  //         |                           |
  //         |                           |
  //        ... (other loop body blocks) |
  //         |                           |
  //         |                           |
  //  +------v--------+                  |
  //  |               |                  |
  //  |    exiting    |                  |
  //  |               |                  |
  //  +------+-+------+   +-----------+  |
  //         | \          |           |  |
  //         |  ---------->   latch   |  |
  //  +------v-----+      |           |  |
  //  |            |      +-----+-----+  |
  //  |    exit    |            \        /
  //  |            |             --------
  //  +------------+
  //
  // Let's say that the loop has two iterations and conditional jump in exiting
  // block is `bne r1, r2, latch`. We have an agreement that in selfcheck mode
  // each iteration of the loop must do the same, difference might be only in
  // the stack as it's a supportive structure. Stack keeps actual values of
  // induction variables which for the loop above will be written to `r1`, `r2`
  // and compared(`bne`).
  //
  // Moving further with the example: at instructions generation stage we'll
  // visit blocks in the following order:
  //   preheader -> header -> other body blocks -> exiting -> latch -> exit.
  //
  // As you can see, we won't execute the second iteration of the loop. When
  // moving interpreter execution to exit block, we have the correct state of
  // registers/memory in interpreter except for r1 and r2 (as we execute only
  // one iteration). However, expected values of r1 and r2 are known statically
  // at loop latcher stage. So, we'll save these values in the map below and
  // write them in interpreter before executing the exit block.
  std::unordered_map<const MachineBasicBlock *, RegToValueType> IncomingValues;
};

class LoopLatcher final
    : public ActiveImmutablePass<MachineFunctionPass, SnippyLoopInfo> {
public:
  static char ID;
  LoopLatcher();

  StringRef getPassName() const override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void processExitingBlock(MachineLoop &ML, MachineBasicBlock &ExitingBlock,
                           MachineBasicBlock &Preheader);
  bool createLoopLatchFor(MachineLoop &ML);
  template <typename R>
  bool createLoopLatchFor(MachineLoop &ML, R &&ConsecutiveLoops);
  auto selectRegsForBranch(const MCInstrDesc &BranchDesc,
                           const MachineBasicBlock &Preheader,
                           const MachineBasicBlock &ExitingBlock,
                           const MCRegisterClass &RegClass);
  MachineInstr &updateLatchBranch(MachineLoop &ML, MachineInstr &Branch,
                                  MachineBasicBlock &Preheader,
                                  ArrayRef<Register> ReservedRegs);

  bool NIterWarned = false;
};

} // namespace snippy
} // namespace llvm
