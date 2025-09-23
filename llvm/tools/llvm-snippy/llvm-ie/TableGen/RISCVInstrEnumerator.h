//===- RISCVInstrEnumerator.h - Generate lists of RISC-V instrs -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_RISCV_INSTR_ENUMERATOR_TABLEGEN_RISCVINSTRENUMERATOR_H
#define LLVM_TOOLS_LLVM_RISCV_INSTR_ENUMERATOR_TABLEGEN_RISCVINSTRENUMERATOR_H

namespace llvm {
class RecordKeeper;
class raw_ostream;
} // namespace llvm

namespace instr_enumerator_tblgen {
bool emitRISCVInstrEnums(llvm::raw_ostream &OS, const llvm::RecordKeeper &RK);
}

#endif
