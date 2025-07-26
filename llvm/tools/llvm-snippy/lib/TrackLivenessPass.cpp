//===-- TrackLivenessPass.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/LivePhysRegs.h"

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/TrackLivenessPass.h"

namespace llvm {
namespace snippy {

#define DEBUG_TYPE "snippy-track-liveness"
#define PASS_DESC "Snippy Track Liveness"

TrackLiveness::TrackLiveness() : MachineFunctionPass(ID) {}

StringRef TrackLiveness::getPassName() const { return PASS_DESC " Pass"; }

bool TrackLiveness::runOnMachineFunction(MachineFunction &MF) {
  if (MF.empty() || !MF.getProperties().hasProperty(
                        MachineFunctionProperties::Property::TracksLiveness))
    return false;

  const auto *TRI = MF.getSubtarget().getRegisterInfo();
  assert(TRI && "register information must be available");

  for (auto *MBB : llvm::post_order(&MF))
    computeAndAddLiveInsForMBB(*MBB, *TRI);

  return true;
}

void TrackLiveness::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void TrackLiveness::computeAndAddLiveInsForMBB(MachineBasicBlock &MBB,
                                               const TargetRegisterInfo &TRI) {
  // This pass can be run multiple times to recompute the live-in registers for
  // MBBs. Therefore, we clear the previous list of live-in registers.
  MBB.clearLiveIns();
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

char TrackLiveness::ID = 0;

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
