//===-- X86.h ---------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../Simulator.h"

#include "snippy/Support/DynLibLoader.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

namespace llvm {
namespace snippy {

struct X86RegisterState : public IRegisterState {
  void loadFromYamlFile(StringRef, WarningsT &) override;
  void saveAsYAMLFile(raw_ostream &) const override;

  void randomize() override;
  bool operator==(const IRegisterState &) const override;
};

std::unique_ptr<SimulatorInterface> createX86Simulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const TargetSubtargetInfo &Subtarget);

} // namespace snippy
} // namespace llvm
