//===-- SimRunner.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SimRunner.h"

#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

namespace llvm {
namespace snippy {

SimRunner::SimRunner(LLVMContext &Ctx, const SnippyTarget &TGT,
                     const TargetSubtargetInfo &Subtarget,
                     SimulationEnvironment SimEnv,
                     ArrayRef<std::string> ModelLibs) {
  Env = std::make_unique<SimulationEnvironment>(std::move(SimEnv));
  assert(!ModelLibs.empty() && "Model lib list must not be empty");

  for (auto &ModelLibName : ModelLibs) {
    auto CfgCopy = Env->SimCfg;
    // Add model plugin name postfix to secondary plugins' trace log files.
    if (!CfgCopy.TraceLogPath.empty() && ModelLibName != ModelLibs.front())
      CfgCopy.TraceLogPath = (CfgCopy.TraceLogPath + Twine(".") +
                              sys::path::filename(ModelLibName))
                                 .str();
    auto Handler = Env->NeedCallbackHandler
                       ? std::make_unique<RVMCallbackHandler>()
                       : nullptr;
    auto Sim = Interpreter::createSimulatorForTarget(
        TGT, Subtarget, CfgCopy, Env->TgtGenCtx, Handler.get(), ModelLibName);
    Interpreters.emplace_back(std::make_unique<Interpreter>(
        Ctx, *Env, std::move(Sim), std::move(Handler)));
    ModelNames.push_back(ModelLibName);
  }

  if (canTrackStepEffects())
    for (auto &I : Interpreters)
      StepEffectsHandles.push_back(
          I->setObserver<StepEffectsObserver>(I->getSupportedCSRs()));
}

SmallVector<StepEffectsObserver *> SimRunner::getStepEffectsObservers() {
  SmallVector<StepEffectsObserver *> Observers;
  for (auto [I, Handle] : zip(Interpreters, StepEffectsHandles))
    Observers.push_back(&I->getObserverByHandle(*Handle));
  return Observers;
}

void SimRunner::run(ProgramCounterType StartPC, ProgramCounterType EndPC) {

  for (auto &I : Interpreters) {
    auto Err = I->setStopModeByPC(EndPC);
    SNIPPY_CHECK_ERROR(Err, "failed to set stop mode and PC");
    Err = I->setPC(StartPC);
    SNIPPY_CHECK_ERROR(Err, "failed to set start PC");
  }

  auto StepEffectsObservers = getStepEffectsObservers();
  bool CheckWholeMemory = StepEffectsObservers.empty();
  checkStates(CheckWholeMemory);

  auto &PrimI = getPrimaryInterpreter();
  PrimI.logMessage("#===Simulation Start===\n");

  for (size_t StepIdx = 0; PrimI.getPC() != EndPC; ++StepIdx) {
    auto PC = PrimI.getPC();
    for (auto *Observer : StepEffectsObservers)
      Observer->clear();
    auto ExecRes = stepInterpreters();

    if (ExecRes != ExecutionResult::Success &&
        ExecRes != ExecutionResult::SimulationExit)
      PrimI.reportSimulationFatalError(
          "Unexpected primary interpreter step result");

    if (StepEffectsObservers.empty())
      checkStates(/* CheckMemory */ false);
    else
      checkStepEffects(StepEffectsObservers, PC, StepIdx);

    if (ExecRes == ExecutionResult::SimulationExit)
      break;
  }

  checkStates(CheckWholeMemory);
}

ExecutionResult SimRunner::stepInterpreters() {
  auto &PrimI = getPrimaryInterpreter();
  auto ExecRes = PrimI.step();
  if (ExecRes == ExecutionResult::FatalError)
    PrimI.reportSimulationFatalError("Primary interpreter step failed");

  for (auto [Num, I] : enumerate(drop_begin(Interpreters))) {
    assert(I.get() != &PrimI);
    if (I->step() == ExecutionResult::FatalError)
      I->reportSimulationFatalError(std::to_string(Num) +
                                    " interpreter step failed");
  }

  return ExecRes;
}

bool SimRunner::canTrackStepEffects() const {
  return Env->NeedCallbackHandler && isCosimulation() &&
         all_of(Interpreters,
                [](auto &I) { return I->modelSupportCallbacks(); });
}

void SimRunner::checkStates(bool CheckMemory) {
  if (!isCosimulation())
    return;
  auto &PI = getPrimaryInterpreter();
  if (!all_of(drop_begin(Interpreters), [&PI, CheckMemory](auto &I) {
        return PI.compareStates(*I, CheckMemory);
      }))
    reportStatesMismatch();
}

void SimRunner::checkStepEffects(ArrayRef<StepEffectsObserver *> Observers,
                                 ProgramCounterType PC, size_t StepIdx) const {
  assert(Observers.size() == Interpreters.size());
  auto &PrimI = *Interpreters.front();
  auto &PrimEffects = Observers.front()->getEffects();
  for (auto Idx : seq<size_t>(1, Interpreters.size())) {
    auto Mismatches = PrimI.compareStepEffects(PrimEffects, *Interpreters[Idx],
                                               Observers[Idx]->getEffects());
    if (Mismatches.empty())
      continue;

    std::string Details;
    raw_string_ostream OS(Details);
    OS << formatv("step {0}, instruction at 0x{1}:\n", StepIdx, utohexstr(PC));
    for (auto &Mismatch : Mismatches)
      OS << formatv("  {0}: {1} ({2}) vs {3} ({4})\n", Mismatch.Location,
                    Mismatch.Value, ModelNames.front(), Mismatch.AnotherValue,
                    ModelNames[Idx]);
    reportStatesMismatch(Details);
  }
}

void SimRunner::reportStatesMismatch(const Twine &Details) const {
  std::string MismatchMessage;
  llvm::raw_string_ostream Stream(MismatchMessage);
  for (auto &I : Interpreters)
    I->dumpCurrentRegStateToStream(Stream);
  snippy::fatal("Interpreters states differ :\n" + Details + MismatchMessage);
}

} // namespace snippy
} // namespace llvm
