//===- main.cpp - RISC-V Instruction Enumerator ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
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

cl::opt<bool> Verbose("verbose",
                      cl::desc("For each filtered instruction, displays the "
                               "categories it belongs to"));
cl::alias VerboseA("v", cl::desc("Alias for --verbose"), cl::aliasopt(Verbose));
cl::opt<bool>
    DisablePseudoOpt("disable-pseudo",
                     cl::desc("Excludes pseudo-instructions from the output"),
                     cl::cat(FeatureOptionsCategory));
} // namespace opts

static std::string formatInstruction(const InstructionEnumerator &IE,
                                     llvm::StringRef Instr) {
  if (!opts::Verbose)
    return Instr.str();

  auto CategoriesOrErr = IE.obtainInstructionCategories(Instr);
  if (auto Error = CategoriesOrErr.takeError())
    return std::string(
        llvm::formatv("{0}:\n\t{1}", Instr, llvm::toString(std::move(Error))));

  auto &Categories = *CategoriesOrErr;
  return Instr.str() + "\n\tCategories: " + llvm::join(Categories, " ");
}

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
  for (auto &&Instr : Instrs)
    llvm::outs() << formatInstruction(*InstrEnumeratorUP.get(), Instr) << "\n";

  terminate();
}
