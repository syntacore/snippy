//===-- SimRunner.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SimRunner.h"
#include "llvm/Support/Path.h"

namespace llvm {
namespace snippy {

SimRunner::SimRunner(LLVMContext &Ctx, const SnippyTarget &TGT,
                     const TargetSubtargetInfo &Subtarget,
                     SimulationEnvironment SimEnv,
                     ArrayRef<std::string> ModelLibs)
    : Ctx(Ctx) {
  Env = std::make_unique<SimulationEnvironment>(std::move(SimEnv));
  assert(!ModelLibs.empty() && "Model lib list must not be empty");

  for (auto &ModelLibName : ModelLibs) {
    auto CfgCopy = Env->SimCfg;
    // Add model plugin name postfix to secondary plugins' trace log files.
    if (!CfgCopy.TraceLogPath.empty() && ModelLibName != ModelLibs.front())
      CfgCopy.TraceLogPath = (CfgCopy.TraceLogPath + Twine(".") +
                              sys::path::filename(ModelLibName))
                                 .str();
    auto Sim = Interpreter::createSimulatorForTarget(
        TGT, Subtarget, CfgCopy, Env->TgtGenCtx, Env->CallbackHandler.get(),
        ModelLibName);
    CoInterp.emplace_back(
        std::make_unique<Interpreter>(Ctx, *Env, std::move(Sim)));
  }
}

ProgramCounterType SimRunner::run(StringRef Programm,
                                  const IRegisterState &InitialRegState) {
  for (auto &I : CoInterp) {
    I->setInitialState(InitialRegState);
    I->loadElfImage(Programm);
  }

  checkStates(/* CheckMemory */ true);

  auto &PrimI = getPrimaryInterpreter();
  PrimI.logMessage("#===Simulation Start===\n");

  while (!PrimI.endOfProg()) {
    if (std::any_of(CoInterp.begin(), CoInterp.end(),
                    [](auto &I) { return !I->step(); })) {
      snippy::warn(WarningName::ModelException, Ctx,
                   "Execution did not reached final instruction",
                   "exception occurred in model");
      return PrimI.getPC();
    }
    checkStates(/* CheckMemory */ false);
  };

  ProgramCounterType LastInstrPC = PrimI.getPC();

  // Execute last instruction
  for (auto &I : CoInterp)
    I->step();

  checkStates(/* CheckMemory */ true);
  return LastInstrPC;
}

void SimRunner::checkStates(bool CheckMemory) {
  if (CoInterp.size() < 2)
    return;
  auto &PI = getPrimaryInterpreter();
  if (std::any_of(CoInterp.begin(), CoInterp.end(),
                  [&PI, CheckMemory](auto &I) {
                    return !PI.compareStates(*I, CheckMemory);
                  }))
    report_fatal_error("Interpreters states differ", false);
}

} // namespace snippy
} // namespace llvm
