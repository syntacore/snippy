//===-- SimulatorContextWrapperPass.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "snippy/ActiveImmutablePass.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Generator/SnippyModule.h"

namespace llvm {
class TargetSubtargetInfo;
namespace snippy {

struct TargetGenContextInterface;
struct GlobalCodeFlowInfo;

class OwningSimulatorContext final : public SimulatorContext {
public:
  OwningSimulatorContext();

  OwningSimulatorContext(OwningSimulatorContext &&Another) = default;
  OwningSimulatorContext &operator=(OwningSimulatorContext &&Another) = default;

  void initialize(SnippyProgramContext &ProgCtx,
                  const TargetSubtargetInfo &SubTgt,
                  const GeneratorSettings &Settings,
                  TargetGenContextInterface &TargetCtx,
                  GlobalCodeFlowInfo &GCFI);

  ~OwningSimulatorContext();
  const SimulatorContext &get() const { return *this; }

private:
  std::unique_ptr<SimRunner> OwnRunner = nullptr;
  std::unique_ptr<SelfCheckInfo> OwnSCI = nullptr;
  std::unique_ptr<Backtrack> OwnBT = nullptr;
};

extern template class GenResultT<OwningSimulatorContext>;

class SimulatorContextWrapper
    : public ActiveImmutablePass<ModulePass, OwningSimulatorContext> {
public:
  static char ID;
  SimulatorContextWrapper(bool DoInit)
      : ActiveImmutablePass<ModulePass, OwningSimulatorContext>(ID),
        DoInit(DoInit){};

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnModule(Module &M) override;

private:
  bool DoInit;
};

class SimulatorContextPreserver : public ModulePass {
public:
  static char ID;
  SimulatorContextPreserver() : ModulePass(ID){};

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnModule(Module &M) override;
};

} // namespace snippy
} // namespace llvm
