//===-- AArch64.h ------------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_TOOLS_SNIPPY_SIMULATOR_TARGETS_AARCH64
#define LLVM_TOOLS_SNIPPY_SIMULATOR_TARGETS_AARCH64

#include "../Simulator.h"

#include "snippy/Support/DynLibLoader.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

namespace llvm {
namespace snippy {

struct AArch64RegisterState : public IRegisterState {
  void loadFromYamlFile(StringRef, WarningsT &,
                        const SnippyTarget *Tgt = nullptr) override;
  void saveAsYAMLFile(raw_ostream &) const override;

  void randomize() override;
  bool operator==(const IRegisterState &) const override;
};

std::unique_ptr<SimulatorInterface> createAArch64Simulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const TargetSubtargetInfo &Subtarget);

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_SNIPPY_SIMULATOR_TARGETS_AARCH64
