//===-- InstructionsPostProcessPass.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm::snippy {
namespace {

#define DEBUG_TYPE "snippy-inst-postprocess"
#define PASS_DESC "Snippy-inst-postprocess"

class InstructionsPostProcess final : public MachineFunctionPass {
  void stripInstructions(MachineFunction &MF) const;

public:
  static char ID;

  InstructionsPostProcess() : MachineFunctionPass(ID) {
    initializeInstructionsPostProcessPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

char InstructionsPostProcess::ID = 0;

} // namespace
} // namespace llvm::snippy

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::InstructionsPostProcess;

INITIALIZE_PASS(InstructionsPostProcess, DEBUG_TYPE, PASS_DESC, true, false)
namespace llvm {
MachineFunctionPass *createInstructionsPostProcessPass() {
  return new InstructionsPostProcess();
}

namespace snippy {

void InstructionsPostProcess::stripInstructions(MachineFunction &MF) const {
  // Reset all pc sections metadata that used as support mark.
  for (MachineBasicBlock &MBB : MF) {
    for (auto &Instr : MBB.instrs()) {
      Instr.setPCSections(MF, nullptr);
    }
  }
}

bool InstructionsPostProcess::runOnMachineFunction(MachineFunction &MF) {
  stripInstructions(MF);
  return false;
}

} // namespace snippy
} // namespace llvm
