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
  // Each interpreter state will be reset before run.
  void run(StringRef Programm, const IRegisterState &InitialRegState,
           ProgramCounterType StartPC);

  // Adds output section name to the sim config
  //  in order to load it later to the model before execution.
  void addNewEnvSection(const std::string &NewSectOutputName) {
    auto &AdditionalSectionsNames = Env->SimCfg.AdditionalSectionsNames;
    if (std::count(AdditionalSectionsNames.begin(),
                   AdditionalSectionsNames.end(), NewSectOutputName) != 0)
      snippy::fatal(formatv("Section {0} has already been added to sim env.",
                            NewSectOutputName));
    AdditionalSectionsNames.push_back(NewSectOutputName);
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
