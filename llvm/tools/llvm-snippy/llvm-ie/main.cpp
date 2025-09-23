//===- main.cpp - RISC-V Instruction Enumerator ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <iostream>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/WithColor.h"

#include "InstructionEnumerator.h"
#include "SystemInitializer.h"
#include "TargetEnum.h"

using namespace llvm;
using namespace llvm_ie;

namespace opts {

cl::OptionCategory FeatureOptionsCategory("Instruction filter options");

cl::opt<TargetEnum> TargetOpt("arch", cl::Required, cl::desc("target"),
                              cl::values(clEnumValN(TargetEnum::eTargetRISCV,
                                                    "riscv", "RISC-V Target")));

cl::opt<bool> MemoryAccessOpt("memory-access",
                              cl::desc("Only memory access instructions"),
                              cl::cat(FeatureOptionsCategory));
cl::opt<bool> ControlFlowOpt("control-flow",
                             cl::desc("Only control flow instructions"),
                             cl::cat(FeatureOptionsCategory));
} // namespace opts

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  initialize();

  auto InstrEnumeratorUP = InstructionEnumerator::findPlugin(opts::TargetOpt);
  if (!InstrEnumeratorUP) {
    llvm::WithColor::error(llvm::errs(), "llvm-ie")
        << "Specified target is unsupported!\n";
    return 1;
  }

  auto Instrs = InstrEnumeratorUP->enumerateInstructions();
  for (auto &&Instr : Instrs) {
    std::cout << Instr.str() << "\n";
  }

  terminate();
}
