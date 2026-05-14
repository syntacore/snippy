//===- AArch64Generated.cpp------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Common/PredicateExpander.h"

#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/SetTheory.h"
#include "llvm/TableGen/TableGenBackend.h"

namespace llvm {
namespace snippy {
namespace {

class SnippyPredicateExpander : public PredicateExpander {
  StringRef ClassPrefix;

  SnippyPredicateExpander(const PredicateExpander &) = delete;
  SnippyPredicateExpander &operator=(const PredicateExpander &) = delete;

public:
  explicit SnippyPredicateExpander(StringRef Target, unsigned Indent = 1)
      : PredicateExpander(Target, Indent) {}

  void expandOpcodeSwitchStatement(raw_ostream &OS,
                                   ArrayRef<const Record *> Cases,
                                   const Record *Default) {
    std::string Buffer;
    raw_string_ostream SS(Buffer);

    SS << "switch(Opcode) {\n";
    for (const Record *Rec : Cases) {
      expandOpcodeSwitchCase(SS, Rec);
      SS << '\n';
    }

    SS << Indent << "default:\n";

    ++Indent;
    SS << Indent;
    expandStatement(SS, Default);
    SS << '\n' << Indent << "} // end of switch-stmt";
    OS << Buffer;
  }
  void expandFunction(raw_ostream &OS, const Record *Rec) {
    const auto &Name = Rec->getValueAsString("FunctionName");
    OS << "inline bool " + Name + "(unsigned Opcode) {\n";
    // TODO: add prologue and refactor
    Rec = Rec->getValueAsDef("Body");
    if (Rec->isSubClassOf("MCOpcodeSwitchStatement")) {
      expandOpcodeSwitchStatement(OS, Rec->getValueAsListOfDefs("Cases"),
                                  Rec->getValueAsDef("DefaultCase"));
    } else {
      // expandReturnStatement(OS, Rec->getValueAsDef("Pred"));
      llvm_unreachable("Snippy Expander does not support sduch function");
    }
    OS << "\n}\n";
  }
};

class SnippyAArch64Emitter {
  raw_ostream &OS;
  const RecordKeeper &Records;

  static constexpr const char *AArch64Namespace = "AArch64";

public:
  SnippyAArch64Emitter(raw_ostream &OS, const RecordKeeper &Records)
      : OS(OS), Records(Records) {}

  void emitLoadStoreFunction() {
    auto Opts = Records.getAllDerivedDefinitions("SnippyFunctionPredicate");
    SnippyPredicateExpander Expander(AArch64Namespace);
    for (auto *R : Opts)
      Expander.expandFunction(OS, R);
  }
  void getExpandedRegs(const Record *CSRSet, SetTheory &RegBank,
                       std::vector<const Record *> &Regs) {
    const SetTheory::RecVec *SaveRegs = RegBank.expand(CSRSet);
    assert(SaveRegs && "Cannot expand CalleeSavedRegs instance");
    Regs.insert(Regs.end(), SaveRegs->begin(), SaveRegs->end());

    if (const DagInit *OPDag =
            dyn_cast<DagInit>(CSRSet->getValueInit("OtherPreserved"))) {
      SetTheory::RecSet OPSet;
      const auto &OPArray = OPSet.getArrayRef();
      Regs.insert(Regs.end(), OPArray.begin(), OPArray.end());
    }
  }
  void emitCalleeSavedReglists() {
    auto CSRSets = Records.getAllDerivedDefinitions("CalleeSavedRegs");
    SetTheory RegBank;
    RegBank.addFieldExpander("CalleeSavedRegs", "SaveList");
    for (const Record *CSRSet : CSRSets) {
      std::string Name = CSRSet->getName().str();

      std::vector<const Record *> AllRegs;
      getExpandedRegs(CSRSet, RegBank, AllRegs);

      OS << "inline std::vector<MCRegister> get" << Name << "AsVector() {\n";
      OS << "  return {\n";

      for (size_t i = 0; i < AllRegs.size(); ++i) {
        OS << "    ";
        if (AllRegs[i]->getValue("Namespace"))
          OS << AllRegs[i]->getValueAsString("Namespace") << "::";
        OS << AllRegs[i]->getName();
        if (i != AllRegs.size() - 1)
          OS << ",";
        OS << "\n";
      }

      OS << "  };\n";
      OS << "}\n\n";
    }
  }
};

} // namespace

bool emitAArch64Generated(raw_ostream &OS, const RecordKeeper &Records) {
  SnippyAArch64Emitter Emitter(OS, Records);

  emitSourceFileHeader("Snippy AArch64 Generated", OS, Records);

  OS << "namespace llvm {\n";
  OS << "namespace snippy {\n";

  Emitter.emitLoadStoreFunction();
  Emitter.emitCalleeSavedReglists();
  OS << "}\n}\n";

  return false;
}

} // namespace snippy
} // namespace llvm
