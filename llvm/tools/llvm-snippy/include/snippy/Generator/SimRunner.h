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

  // Preform co-simulation run and returns PC before final instruction.
  // Each interpreter state will be reset before run.
  ProgramCounterType run(StringRef Programm,
                         const IRegisterState &InitialRegState);

  // Adds output section name to the sim config
  //  in order to load it later to the model before execution.
  void addNewEnvSection(const std::string &NewSectOutputName) {
    auto &AdditionalSectionsNames = Env->SimCfg.AdditionalSectionsNames;
    if (std::count(AdditionalSectionsNames.begin(),
                   AdditionalSectionsNames.end(), NewSectOutputName) != 0)
      report_fatal_error("Section " + Twine(NewSectOutputName) +
                             " has already been added to sim env",
                         false);
    AdditionalSectionsNames.push_back(NewSectOutputName);
  }

private:
  void checkStates(bool CheckMemory);

  LLVMContext &Ctx;
  std::unique_ptr<SimulationEnvironment> Env;
  std::vector<std::unique_ptr<Interpreter>> CoInterp;
};

} // namespace snippy
} // namespace llvm
