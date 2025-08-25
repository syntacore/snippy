//===- RISCVGenerated.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Common/CodeGenInstruction.h"

#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

namespace llvm {
namespace snippy {

namespace {

static constexpr unsigned DefaultIndent = 2;
static constexpr unsigned DoubleIndent = 2 * DefaultIndent;

class SnippyRISCVEmitter {
  raw_ostream &OS;
  const RecordKeeper &Records;

  static constexpr const char *RISCVNamespace = "RISCV";

  void emitExcludedOpcodesFuncDecl() const {
    OS << "bool snippyRISCVIsOpcodeExcluded(unsigned Opc)";
  }

public:
  SnippyRISCVEmitter(raw_ostream &OS, const RecordKeeper &Records)
      : OS(OS), Records(Records) {}

  void emitExcludedOpcodes() {
    OS << "\n#ifdef SNIPPY_RISCV_EXCLUDED_OPCODES_DECL\n";
    OS << "#undef SNIPPY_RISCV_EXCLUDED_OPCODES_DECL\n\n";
    emitExcludedOpcodesFuncDecl();
    OS << ";\n\n";
    OS << "#endif\n";

    OS << "\n#ifdef SNIPPY_RISCV_EXCLUDED_OPCODES_DEF\n";
    OS << "#undef SNIPPY_RISCV_EXCLUDED_OPCODES_DEF\n\n";
    auto Opts = Records.getAllDerivedDefinitions("Instruction");
    emitExcludedOpcodesFuncDecl();
    OS << " {\n";
    OS.indent(DefaultIndent) << "switch (Opc) {\n";
    for (auto *R : Opts) {
      assert(R);
      auto Ins = CodeGenInstruction(R);

      // We are only interested in RISCV opcodes.
      if (Ins.Namespace != RISCVNamespace)
        continue;

      // Only CodeGenOnly and AsmParserOnly instructions are excluded.
      // Pseudos are handled manually, so they should not be disallowed here.
      bool IsExcluded =
          !Ins.isPseudo && (Ins.isCodeGenOnly || Ins.isAsmParserOnly);

      if (!IsExcluded)
        continue;

      OS.indent(DefaultIndent)
          << "case " << Ins.Namespace << "::" << R->getName() << ":\n";
    }
    OS.indent(DoubleIndent) << "return true;\n";
    OS.indent(DefaultIndent) << "default:\n";
    OS.indent(DoubleIndent) << "return false;\n";
    OS.indent(DefaultIndent) << "}\n";
    OS << "}\n\n#endif\n";
  }
};

} // namespace

bool emitRISCVGenerated(raw_ostream &OS, const RecordKeeper &Records) {
  SnippyRISCVEmitter Emitter(OS, Records);

  emitSourceFileHeader("Snippy RISCV Generated", OS, Records);

  OS << "namespace llvm {\n";
  OS << "namespace snippy {\n";

  Emitter.emitExcludedOpcodes();

  OS << "}\n}\n";

  return false;
}

} // namespace snippy
} // namespace llvm
