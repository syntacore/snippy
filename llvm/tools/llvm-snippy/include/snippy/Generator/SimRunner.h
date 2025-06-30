//===-- SimRunner.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// SimRunner is used to perform co-simulation runs with additional
/// cross-simulator state match checks.
///
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/Interpreter.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/Support/FormatVariadic.h"

namespace llvm {
namespace snippy {

class SimRunner {
public:
  // Constructructs SimRunner with interpreters that share single
  // SimulationEnvironment. For each interpreter simulator is constructed using
  // model loaded from corresponding path in ModelLibs.
  SimRunner(LLVMContext &Ctx, const SnippyTarget &TGT,
            const TargetSubtargetInfo &Subtarget, SimulationEnvironment Env,
            ArrayRef<std::string> ModelLibs);

  // First interpreter in list considered a primary one. It can be accessed
  // and used freely.
  Interpreter &getPrimaryInterpreter() {
    assert(!CoInterp.empty() && "At least one interpeter expected");
    return *CoInterp.front();
  }

  // Preform co-simulation run.
  void run(ProgramCounterType StartPC, ProgramCounterType EndPC);
  // Loads image of program into each interpreter.

  Error loadElfSectionsToModel(const ParsedElf &ElfData, bool InitBSS) {
    for (auto &I : CoInterp)
      if (auto Err = I->loadElfImage(ElfData, InitBSS))
        return Err;

    return Error::success();
  }

  void resetState(const SnippyProgramContext &ProgCtx, bool DoMemReset) {
    for (auto &&PI : CoInterp)
      PI->resetState(ProgCtx, DoMemReset);
  }
  auto &getSimConfig() & {
    assert(Env);
    return Env->SimCfg;
  }

private:
  void checkStates(bool CheckMemory);

  std::unique_ptr<SimulationEnvironment> Env;
  std::vector<std::unique_ptr<Interpreter>> CoInterp;
};

} // namespace snippy
} // namespace llvm
