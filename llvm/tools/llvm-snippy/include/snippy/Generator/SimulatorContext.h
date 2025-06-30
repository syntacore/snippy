//===-- SimulatorContext.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once
#include "snippy/Config/Config.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/SimRunner.h"

#include "snippy/Support/Options.h"

namespace llvm {
namespace snippy {

class Backtrack;
struct SelfCheckInfo;
class SnippyProgramContext;

struct SimulatorContext {
  SimRunner *Runner = nullptr;
  SelfCheckInfo *SCI = nullptr;
  Backtrack *BT = nullptr;
  SimulatorContext() = default;
  virtual ~SimulatorContext() = default;

  void enableTrackingMode() { HasTrackingMode = true; }
  Interpreter &getInterpreter() const {
    assert(Runner);
    return Runner->getPrimaryInterpreter();
  }

  SimRunner &getSimRunner() const {
    assert(Runner);
    return *Runner;
  }
  bool hasTrackingMode() const { return HasTrackingMode; }
  bool hasModel() const { return Runner; }
  void disableTrackingMode() const { HasTrackingMode = false; }
  void notifyMemUpdate(uint64_t Addr, const APInt &Value) const {
    getInterpreter().writeMem(Addr, Value);
  }

  struct RunInfo {
    StringRef ImageToRun;
    SnippyProgramContext &ProgCtx;
    SnippyModule &MainModule;
    StringRef EntryPointName;
    StringRef InitialRegStateOutputYaml;
    StringRef FinalRegStateOutputYaml;
    bool SelfcheckCheckMem;
    bool NeedMemoryReset;
    std::vector<std::string> DumpMemorySection;
    StringRef MemorySectionFile;
    StringRef BaseFilename;
  };

  Error runSimulator(const RunInfo &RI);

private:
  void checkMemStateAfterSelfcheck(SnippyProgramContext &ProgCtx) const;
  mutable bool HasTrackingMode = false;
};

} // namespace snippy
} // namespace llvm
