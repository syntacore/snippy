//===- SnippyOptions.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SnippyOptions.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

namespace llvm {
namespace snippy {

namespace {

enum OptionKind { KindPlain, KindList, KindAlias, InvalidKind };

static std::string recordToString(const Record &R) {
  std::string Record;
  raw_string_ostream OSS(Record);
  OSS << R;
  return Record;
}

static OptionKind getOptionKind(const Record &R) {
  auto Kind = StringSwitch<OptionKind>(R.getName())
                  .Case("KindPlain", KindPlain)
                  .Case("KindList", KindList)
                  .Case("KindAlias", KindAlias)
                  .Default(InvalidKind);
  if (Kind == InvalidKind)
    PrintFatalError(
        Twine("Invalid 'Kind' for Record: ").concat(recordToString(R)));
  return Kind;
}

class SnippyOptionsEmitter {
  raw_ostream &OS;
  RecordKeeper &Records;

  void emitSnippyOption(const Record &R) {
    const auto *KindRecord = R.getValueAsDef("Kind");
    auto Kind = getOptionKind(*KindRecord);

    auto GetTypeName = [&]() {
      const auto *Type = R.getValueAsDef("Type");
      return Type->getValueAsString("Type");
    };

    OS << "static ";
    switch (Kind) {
    case OptionKind::KindPlain:
      OS << "snippy::opt<" << GetTypeName() << ">";
      break;
    case OptionKind::KindList:
      OS << "snippy::opt_list<" << GetTypeName() << ">";
      break;
    case OptionKind::KindAlias:
      OS << "snippy::alias";
      break;
    case OptionKind::InvalidKind:
      llvm_unreachable("Invalid option kind");
    }

    OS << "\n";

    OS.indent(2) << R.getName() << "(";

    OS << "\"" << R.getValueAsString("Name") << "\"";
    if (auto Desc = R.getValueAsOptionalString("Description"); Desc)
      OS << ", cl::desc(\"" << *Desc << "\")";

    if (auto Desc = R.getValueAsOptionalString("ValueDescription"); Desc)
      OS << ", cl::value_desc(\"" << *Desc << "\")";

    if (auto Val = R.getValueAsOptionalString("DefaultValue"); Val) {
      auto Quote = GetTypeName() == "std::string";
      OS << ", cl::init(";
      if (Quote)
        OS << '"';
      OS << *Val;
      if (Quote)
        OS << '"';

      OS << ")";
    }

    if (const auto *Alias = R.getValueAsOptionalDef("Alias")) {
      if (Kind != KindAlias)
        PrintFatalError(R.getLoc(), "Alias can only be used with KindAlias");
      OS << ", snippy::aliasopt(" << Alias->getName() << ")";
    }

    {
      bool Unset = false;
      if (R.getValueAsBitOrUnset("CommaSeparated", Unset))
        OS << ", cl::CommaSeparated";
      if (!Unset && Kind != KindList)
        PrintFatalError(R.getLoc(),
                        "CommaSeparated can only be used with KindList");
    }

    if (const auto *Group = R.getValueAsOptionalDef("Group"))
      OS << ", cl::cat(" << Group->getName() << ")";

    if (R.getValueAsBit("Hidden"))
      OS << ", cl::Hidden";

    OS << ");" << "\n\n";
  }

public:
  SnippyOptionsEmitter(raw_ostream &OS, RecordKeeper &Records)
      : OS(OS), Records(Records) {}

  void emitOptions() {
    for (const auto *R : Records.getAllDerivedDefinitions("Option")) {
      assert(R);
      emitSnippyOption(*R);
    }
  }
};

} // namespace

bool emitSnippyOptions(raw_ostream &OS, RecordKeeper &Records) {
  SnippyOptionsEmitter Emitter(OS, Records);

  emitSourceFileHeader("Snippy options", OS, Records);

  OS << "\n#ifdef GEN_SNIPPY_OPTIONS_DEF\n";
  OS << "#undef GEN_SNIPPY_OPTIONS_DEF\n\n";
  Emitter.emitOptions();
  OS << "#endif\n";

  return false;
}

} // namespace snippy
} // namespace llvm
