//===- SnippyOptions.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SnippyOptions.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

namespace llvm {
namespace snippy {

namespace {

static constexpr unsigned DefaultIndent = 2;
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

StringRef getPrimitiveTypeName(const Record &R) {
  const auto *Type = R.getValueAsDef("Type");
  return Type->getValueAsString("Type");
}

std::string getTypeName(const Record &R) {
  auto Type = getPrimitiveTypeName(R);
  const auto *KindRecord = R.getValueAsDef("Kind");
  auto Kind = getOptionKind(*KindRecord);
  if (Kind == OptionKind::KindList)
    return formatv("std::vector<{0}>", Type).str();
  return Type.str();
}

void emitSnippyOptType(raw_ostream &OS, const Record &R) {
  const auto *KindRecord = R.getValueAsDef("Kind");
  auto Kind = getOptionKind(*KindRecord);
  switch (Kind) {
  case OptionKind::KindPlain:
    OS << "snippy::opt<" << getPrimitiveTypeName(R) << ">";
    break;
  case OptionKind::KindList:
    OS << "snippy::opt_list<" << getPrimitiveTypeName(R) << ">";
    break;
  case OptionKind::KindAlias:
    OS << "snippy::alias";
    break;
  case OptionKind::InvalidKind:
    llvm_unreachable("Invalid option kind");
  }
}

class SnippyOptionsEmitter {
  raw_ostream &OS;
  RecordKeeper &Records;

  void emitSnippyOptionDeclaration(const Record &R) {
    OS << "extern ";
    emitSnippyOptType(OS, R);
    OS << ' ' << R.getName() << ";\n";
  }

  void emitSnippyOption(const Record &R) {
    emitSnippyOptType(OS, R);
    OS << "\n";

    OS.indent(DefaultIndent) << R.getName() << "(";

    OS << "\"" << R.getValueAsString("Name") << "\"";
    if (auto Desc = R.getValueAsOptionalString("Description"); Desc)
      OS << ", cl::desc(\"" << *Desc << "\")";

    if (auto Desc = R.getValueAsOptionalString("ValueDescription"); Desc)
      OS << ", cl::value_desc(\"" << *Desc << "\")";

    if (auto Val = R.getValueAsOptionalString("DefaultValue"); Val) {
      auto Quote = getPrimitiveTypeName(R) == "std::string";
      OS << ", cl::init(";
      if (Quote)
        OS << '"';
      OS << *Val;
      if (Quote)
        OS << '"';

      OS << ")";
    }
    const auto *KindRecord = R.getValueAsDef("Kind");
    auto Kind = getOptionKind(*KindRecord);

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

    // Use constant reference because cl::callback requires function
    // that has lvalue reference as an argument
    if (auto Callback = R.getValueAsOptionalString("Callback")) {
      OS << ", cl::callback([](const " << getPrimitiveTypeName(R) << " &"
         << R.getName() << ") { " << StringRef(*Callback).trim('"') << " })";
    }

    OS << ");" << "\n\n";
  }

public:
  SnippyOptionsEmitter(raw_ostream &OS, RecordKeeper &Records)
      : OS(OS), Records(Records) {}

  void emitOptions() {
    auto Opts = Records.getAllDerivedDefinitions("Option");
    for (const auto *R : Opts) {
      assert(R);
      emitSnippyOptionDeclaration(*R);
    }
    for (const auto *R : Opts) {
      assert(R);
      emitSnippyOption(*R);
    }
  }
};

class SnippyOptionStructEmitter {
  raw_ostream &OS;
  RecordKeeper &Records;
  MapVector<StringRef, const Record *> Groups;

  void emitSnippyOption(const Record &R) {
    OS.indent(DefaultIndent) << getTypeName(R) << " " << R.getName() << ";\n";
    OS.indent(DefaultIndent) << "bool " << R.getName() << "Specified;\n";
  }

  void collectAllOptionGroups() {
    auto Opts = Records.getAllDerivedDefinitions("Option");
    for (auto *O : Opts) {
      auto *Group = O->getValueAsDef("Group");
      Groups.try_emplace(Group->getName(), Group);
    }
  }

public:
  static std::string getOptionsStructName(const Record &Group) {
    if (auto StructName = Group.getValueAsOptionalString("StructName"))
      return StructName->str();
    return "Struct" + Group.getName().str();
  }

  SnippyOptionStructEmitter(raw_ostream &OS, RecordKeeper &Records)
      : OS(OS), Records(Records) {
    collectAllOptionGroups();
  }

  void emitOptions() {
    auto Opts = Records.getAllDerivedDefinitions("Option");
    for (auto &[GroupName, Group] : Groups) {
      auto StructName = getOptionsStructName(*Group);
      OS << "struct " << StructName << " {\n";
      for (const auto *R : Opts) {
        const auto *KindRecord = R->getValueAsDef("Kind");
        auto Kind = getOptionKind(*KindRecord);
        if (Kind != OptionKind::KindAlias &&
            R->getValueAsDef("Group")->getName() == GroupName)
          emitSnippyOption(*R);
      }
      OS << "};\n";
    }
  }
  void emitCopyOptionIfNeeded(const Record &R) {
    const auto *KindRecord = R.getValueAsDef("Kind");
    auto Kind = getOptionKind(*KindRecord);
    if (Kind == OptionKind::KindAlias)
      return;
    OS.indent(DefaultIndent)
        << formatv("Res.{0} = static_cast<const CommandOption<{1}> "
                   "&>(Storage.get(\"{2}\")).Val;\n",
                   R.getName(), getTypeName(R), R.getValueAsString("Name"));
    OS.indent(DefaultIndent)
        << formatv("Res.{0}Specified = Storage.get(\"{1}\").isSpecified();\n",
                   R.getName(), R.getValueAsString("Name"));
  }
  void emitOptionsCopyFunction() {
    auto Opts = Records.getAllDerivedDefinitions("Option");
    for (auto &[GroupName, Group] : Groups) {
      auto StructName = getOptionsStructName(*Group);
      OS << "inline " << StructName << " copyOptionsTo" << StructName
         << "() {\n";
      OS.indent(DefaultIndent) << StructName << " Res;\n";
      OS.indent(DefaultIndent)
          << "auto &Storage = OptionsStorage::instance();\n";
      auto GroupedOpts =
          make_filter_range(Opts, [Name = StringRef(GroupName)](const auto *O) {
            return O->getValueAsDef("Group")->getName() == Name;
          });
      for (const auto *R : GroupedOpts)
        emitCopyOptionIfNeeded(*R);
      OS.indent(DefaultIndent) << "return Res;\n";
      OS << "}\n\n";
    }
  }
};
} // namespace

bool emitSnippyOptions(raw_ostream &OS, RecordKeeper &Records) {
  SnippyOptionsEmitter Emitter(OS, Records);

  emitSourceFileHeader("Snippy options", OS, Records);

  OS << "\n#ifdef GEN_SNIPPY_OPTIONS_DEF\n";
  OS << "#undef GEN_SNIPPY_OPTIONS_DEF\n\n";
  OS << "namespace {\n";
  Emitter.emitOptions();
  OS << "} // namespace\n";
  OS << "#endif\n";

  return false;
}

bool emitSnippyOptionsStruct(raw_ostream &OS, RecordKeeper &Records) {
  SnippyOptionStructEmitter Emitter(OS, Records);
  emitSourceFileHeader("Snippy options struct", OS, Records);
  OS << "\n#ifdef GEN_SNIPPY_OPTIONS_STRUCT_DEF\n";
  OS << "#undef GEN_SNIPPY_OPTIONS_STRUCT_DEF\n\n";
  Emitter.emitOptions();
  OS << '\n';
  Emitter.emitOptionsCopyFunction();
  OS << "#endif\n";
  return false;
}
} // namespace snippy
} // namespace llvm
