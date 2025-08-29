//===-- Selfcheck.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/Selfcheck.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Generator/LLVMState.h"

#include "llvm/Support/YAMLTraits.h"

#define DEBUG_TYPE "snippy-selfcheck-config"

namespace llvm {
void yaml::MappingTraits<snippy::SelfcheckConfig>::mapping(
    yaml::IO &IO, snippy::SelfcheckConfig &Selfcheck) {
  IO.mapRequired("mode", Selfcheck.Mode);
  IO.mapOptional("period", Selfcheck.Period);
  IO.mapOptional("selfcheck-gv", Selfcheck.SelfcheckGV);

  if (!IO.outputting()) {
    auto *Ctx = IO.getContext();
    assert(
        Ctx &&
        "To parse or output SelfcheckTargetConfig provide ConfigIOContext as "
        "context for yaml::IO");
    const auto &Tgt = static_cast<const snippy::ConfigIOContext *>(Ctx)
                          ->State.getSnippyTarget();
    Selfcheck.STC = Tgt.createSelfcheckTargetConfig();
  }
  IO.mapOptional("selfcheck-td-options", *Selfcheck.STC);
}

std::string yaml::MappingTraits<snippy::SelfcheckConfig>::validate(
    yaml::IO &IO, snippy::SelfcheckConfig &Selfcheck) {
  // Target independent validation part

  if (Selfcheck.isCheckSumSelfcheckModeEnabled() && Selfcheck.SelfcheckGV)
    return "'selfcheck-gv' doesn't make sense with selfcheck checksum mode "
           "enabled";

  // Target dependent validation part
  auto *Ctx = IO.getContext();
  const auto &Tgt = static_cast<const snippy::ConfigIOContext *>(Ctx)
                        ->State.getSnippyTarget();
  return Tgt.validateSelfcheckConfig(Selfcheck);
}

void yaml::MappingTraits<snippy::SelfcheckTargetConfigInterface>::mapping(
    yaml::IO &IO, snippy::SelfcheckTargetConfigInterface &STgtCtx) {
  STgtCtx.mapConfig(IO);
}
} // namespace llvm
