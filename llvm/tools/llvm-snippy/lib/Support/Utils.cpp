//===-- Utils.cpp -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/Utils.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"

namespace llvm {
namespace snippy {

bool checkMetadata(const MachineInstr &MI, SnippyMetadata M) {
  // FIXME: we do not have appropriate way to check metadata.
  MDNode *Node = MI.getPCSections();
  if (!Node)
    return false;
  auto *I = llvm::find_if(Node->operands(), [M](auto &&Oper) {
    auto *MDStr = dyn_cast<MDString>(Oper);
    return MDStr && MDStr->getString() == detail::getStrMetadata(M);
  });
  return I != Node->operands().end();
}

void writeFile(StringRef Path, StringRef Data) {
  std::error_code EC;
  raw_fd_ostream File(Path, EC);
  if (EC)
    snippy::fatal(EC.message() + ": " + Path);
  File.write(Data.data(), Data.size());
  File.flush();
  if (File.has_error()) {
    EC = File.error();
    snippy::fatal(EC.message() + ": " + Path);
  }
}

Error checkedWriteToOutput(const Twine &OutputFileName,
                           std::function<Error(raw_ostream &)> Write) {
  std::string Name = OutputFileName.str();
  if (Name == "-")
    return Write(outs());

  std::error_code EC;
  raw_fd_ostream Out(Name, EC);

  if (EC)
    return createFileError(OutputFileName, EC);

  Error E = Write(Out);
  Out.flush();

  if (Out.has_error()) {
    std::error_code EC = Out.error();
    Out.clear_error();
    return joinErrors(std::move(E), createFileError(OutputFileName, EC));
  }

  return E;
}

std::string addExtensionIfRequired(StringRef StrRef, std::string Ext) {
  if (StrRef.ends_with(Ext))
    return StrRef.str();

  auto String = StrRef.str();
  String.append(Ext);
  return String;
}

void setAsSupportInstr(MachineInstr &MI, LLVMContext &Ctx) {
  if (checkMetadata(MI, SnippyMetadata::Support))
    return;

  // FIXME: we shouldn't overwrite PC sections here but now we have only one
  // Metadata ...
  addSnippyMetadata(MI, *MI.getParent()->getParent(), Ctx,
                    SnippyMetadata::Support);
}

std::string floatToString(APFloat F, unsigned Precision) {
  llvm::SmallString<10> S;
  F.toString(S, Precision);
  return S.str().str();
}

unsigned getAutoSenseRadix(StringRef &Str) {
  if (Str.empty())
    return 10;

  if (Str.consume_front_insensitive("0x"))
    return 16;

  if (Str.consume_front_insensitive("0b"))
    return 2;

  if (Str.consume_front("0o"))
    return 8;

  if (Str[0] == '0' && Str.size() > 1 && isDigit(Str[1])) {
    Str = Str.substr(1);
    return 8;
  }

  return 10;
}

void replaceAllSubstrs(std::string &Str, StringRef What, StringRef With) {
  for (auto Pos = Str.find(What); std::string::npos != Pos;
       Pos = Str.find(What, Pos + With.size()))
    Str.replace(Pos, What.size(), With);
}

} // namespace snippy
} // namespace llvm
