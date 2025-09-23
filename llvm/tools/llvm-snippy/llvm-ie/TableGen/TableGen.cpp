//===--- TableGen.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/TableGen/Main.h"

#include "RISCVInstrEnumerator.h"

using namespace llvm;
using namespace instr_enumerator_tblgen;

enum class Target {
  RISCV,
};

cl::opt<Target>
    TargetOpt("arch", cl::Required, cl::desc("target"),
              cl::values(clEnumValN(Target::RISCV, "riscv", "RISC-V Target")));

static bool instrEnumeratorTableGenMain(llvm::raw_ostream &OS,
                                        const RecordKeeper &RK) {
  switch (TargetOpt) {
  case Target::RISCV:
    return emitRISCVInstrEnums(OS, RK);
  }
  return false;
}

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv);
  return TableGenMain(argv[0], &instrEnumeratorTableGenMain);
}
