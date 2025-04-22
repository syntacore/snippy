//===-- TrackLivenessPass.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <llvm/CodeGen/LivePhysRegs.h>

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"

namespace llvm {
namespace snippy {
namespace {

#define DEBUG_TYPE "snippy-track-liveness"
#define PASS_DESC "Snippy Track Liveness"

class TrackLiveness : public MachineFunctionPass {

public:
  static char ID;

  TrackLiveness() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (MF.empty() || !MF.getProperties().hasProperty(
                          MachineFunctionProperties::Property::TracksLiveness))
      return false;

    const auto *TRI = MF.getSubtarget().getRegisterInfo();
    assert(TRI && "register information must be available");

    ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
    for (auto *MBB : RPOT)
      computeAndAddLiveInsForMBB(*MBB, *TRI);

    return true;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<GeneratorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  void computeAndAddLiveInsForMBB(MachineBasicBlock &MBB,
                                  const TargetRegisterInfo &TRI) {
    LivePhysRegs LiveRegs(TRI);
    LiveRegs.addLiveOuts(MBB);

    // Traverse instructions backward to compute live-ins
    for (auto &&MI : llvm::reverse(MBB))
      LiveRegs.stepBackward(MI);

    // Add live-ins to the block
    for (auto &&Reg : llvm::make_filter_range(
             LiveRegs, [&](auto &&Reg) { return !MBB.isLiveIn(Reg); }))
      MBB.addLiveIn(Reg);
  }
};

char TrackLiveness::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::TrackLiveness;

INITIALIZE_PASS(TrackLiveness, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createTrackLivenessPass() {
  return new snippy::TrackLiveness;
}

} // namespace llvm
