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

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SIMRUNNER_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SIMRUNNER_H

#include "snippy/Generator/Interpreter.h"
#include "snippy/Support/DiagnosticInfo.h"

namespace llvm {
namespace snippy {

class MemoryManager;

class SimRunner {
public:
  // Constructs SimRunner with interpreters that share single
  // SimulationEnvironment. For each interpreter simulator is constructed using
  // model loaded from corresponding path in ModelLibs.
  SimRunner(LLVMContext &Ctx, const SnippyTarget &TGT,
            const TargetSubtargetInfo &Subtarget, SimulationEnvironment Env,
            ArrayRef<std::string> ModelLibs);

  // First interpreter in list considered a primary one. It can be accessed
  // and used freely.
  Interpreter &getPrimaryInterpreter() {
    assert(!Interpreters.empty() && "At least one interpreter expected");
    return *Interpreters.front();
  }

  // Preform co-simulation run.
  void run(ProgramCounterType StartPC, ProgramCounterType EndPC);

  // Loads image of program into each interpreter.
  Error loadElfSectionsToModel(const ParsedElf &ElfData, bool InitBSS) {
    for (auto &I : Interpreters)
      if (auto Err = I->loadElfImage(ElfData, InitBSS))
        return Err;

    return Error::success();
  }

  void resetState(const SnippyProgramContext &ProgCtx, bool FullReset) {
    for (auto &I : Interpreters)
      I->resetState(ProgCtx, FullReset);
  }
  // Initializes memory in all interpreters.
  template <typename ItT>
  void initInterpretersMemory(const ItT MemStateBeg, const ItT MemStateEnd) {
    for_each(
        Interpreters,
        [MemStateBeg, MemStateEnd](std::unique_ptr<Interpreter> &InterpPtr) {
          std::for_each(MemStateBeg, MemStateEnd,
                        [&InterpPtr](const auto &SectData) {
                          auto Err = InterpPtr->writeSection(SectData);
                          SNIPPY_CHECK_ERROR(Err, "failed to write section");
                        });
        });
  }
  auto &getSimConfig() & {
    assert(Env);
    return Env->SimCfg;
  }

private:
  bool isCosimulation() const { return Interpreters.size() > 1; }

  bool canTrackStepEffects() const;

  ExecutionResult stepInterpreters();

  SmallVector<StepEffectsObserver *> getStepEffectsObservers();

  void checkStates(bool CheckMemory);

  void checkStepEffects(ArrayRef<StepEffectsObserver *> Observers,
                        ProgramCounterType PC, size_t StepIdx) const;

  [[noreturn]] void reportStatesMismatch(const Twine &Details = "") const;

  std::unique_ptr<SimulationEnvironment> Env;
  // Interpreters of the run. The first one is the primary interpreter,
  // the others are compared against it. Must be declared after Env.
  std::vector<std::unique_ptr<Interpreter>> Interpreters;
  std::vector<std::string> ModelNames;
  // Handles of the observers that record what each model reports during a
  // step. Empty when some model does not report its updates.
  std::vector<
      std::unique_ptr<RVMCallbackHandler::ObserverHandle<StepEffectsObserver>>>
      StepEffectsHandles;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_SIMRUNNER_H
