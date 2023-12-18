//===-- PrintMachineInstrsPass.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "snippy-print-machine-instrs"
#define PASS_DESC "Snippy Print Generated Machine Instructions"

namespace llvm {
namespace snippy {
namespace {

class PrintMachineInstrs final : public MachineFunctionPass {
  raw_ostream &OS;
  std::string Instrs;
  int MICounter = 0;

public:
  static char ID;

  PrintMachineInstrs() : MachineFunctionPass(ID), OS(outs()) {}
  PrintMachineInstrs(raw_ostream &OS) : MachineFunctionPass(ID), OS(OS) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool doInitialization(Module &M) override {
    MICounter = 0;
    return false;
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  bool doFinalization(Module &M) override {
    OS << Instrs;
    OS.flush();
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char PrintMachineInstrs::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::PrintMachineInstrs;

INITIALIZE_PASS(PrintMachineInstrs, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createPrintMachineInstrsPass(raw_ostream &OS) {
  return new PrintMachineInstrs(OS);
}

namespace snippy {

bool PrintMachineInstrs::runOnMachineFunction(MachineFunction &MF) {
  std::string S;
  raw_string_ostream SOS(S);
  SOS << "Machine Instruction dump for <" << MF.getName() << "> start\n";
  for (const auto &MBB : MF) {
    for (const auto &MI : MBB.instrs()) {
      SOS << "Generated MI_" << MICounter << ": ";
      MI.print(SOS);
      ++MICounter;
    }
  }
  SOS << "Machine Instruction dump for <" << MF.getName() << "> end\n";
  Instrs.append(SOS.str());
  return false;
}

} // namespace snippy
} // namespace llvm
