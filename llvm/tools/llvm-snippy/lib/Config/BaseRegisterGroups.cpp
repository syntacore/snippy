//===-- BaseRegisterGroups.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/BaseRegisterGroups.h"

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/OpcodeCache.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"

namespace llvm {
namespace snippy {

UniqueOpcodesTy opcodeGroupFromNames(const SList &Names,
                                     const OpcodeCache &OpCC) {
  UniqueOpcodesTy Res;
  llvm::transform(Names, std::inserter(Res, Res.end()),
                  [&OpCC](const std::string &Name) {
                    auto Opt = OpCC.code(Name);
                    if (!Opt.has_value())
                      snippy::fatal(Twine("Unknown instruction \"") + Name +
                                    "\" in base-register-groups");
                    return *Opt;
                  });
  return Res;
}

std::vector<SList> denormalizeBaseRegisterGroups(const OpcodeGroupsTy &Groups,
                                                 const OpcodeCache &OpCC) {
  std::vector<SList> Result;
  llvm::transform(Groups, std::back_inserter(Result), [&OpCC](const auto &Set) {
    SList Res;
    llvm::transform(Set, std::back_inserter(Res),
                    [&OpCC](auto Val) { return OpCC.name(Val).str(); });
    return Res;
  });
  return Result;
}

} // namespace snippy
} // namespace llvm
