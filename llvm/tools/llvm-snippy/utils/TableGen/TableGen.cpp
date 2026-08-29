//===- TableGen.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Emitters.h"
#include "SnippyOptions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Main.h"
#include "llvm/TableGen/Record.h"

namespace llvm {
namespace snippy {

enum ActionType {
  PrintRecords,
  DumpJSON,
  GenOptions,
  GenOptionsStruct,
  GenRISCV,
  GenAArch64Operands,
  GenAArch64,
  GenRISCVOperands
};

static cl::opt<ActionType>
    Action(cl::desc("Action to perform:"),
           cl::values(clEnumValN(PrintRecords, "print-records",
                                 "Print all records to stdout (default)"),
                      clEnumValN(DumpJSON, "dump-json",
                                 "Dump all records as machine-readable JSON"),
                      clEnumValN(GenOptionsStruct, "gen-options-struct",
                                 "Generate option struct definition"),
                      clEnumValN(GenOptions, "gen-options",
                                 "Generate option definitions"),
                      clEnumValN(GenRISCV, "gen-riscv",
                                 "Generate RISCV Target definitions"),
                      clEnumValN(GenAArch64Operands, "gen-aarch64-operands",
                                 "Generate Aarch64 operands"),
                      clEnumValN(GenAArch64, "gen-aarch64",
                                 "Generate AArch64 Target definitions"),
                      clEnumValN(GenRISCVOperands, "gen-riscv-operands",
                                 "Generate RISCV operands")));

static bool snippyTableGenMain(raw_ostream &OS, const RecordKeeper &Records) {
  switch (Action) {
  case PrintRecords:
    OS << Records; // No argument, dump all contents
    break;
  case DumpJSON:
    EmitJSON(Records, OS);
    break;
  case GenOptions:
    emitSnippyOptions(OS, Records);
    break;
  case GenOptionsStruct:
    emitSnippyOptionsStruct(OS, Records);
    break;
  case GenRISCV:
    emitRISCVGenerated(OS, Records);
    break;
  case GenAArch64Operands:
    emitAArch64Operands(OS, Records);
    break;
  case GenAArch64:
    emitAArch64Generated(OS, Records);
    break;
  case GenRISCVOperands:
    emitRISCVOperands(OS, Records);
    break;
  }

  return false;
}

} // namespace snippy
} // namespace llvm

int main(int argc, char **argv) {
  using namespace llvm;
  using namespace llvm::snippy;
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram StackTrace(argc, argv);
  cl::ParseCommandLineOptions(argc, argv);
  llvm_shutdown_obj Shutdown;
  return TableGenMain(argv[0], &snippyTableGenMain);
}
