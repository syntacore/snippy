//===-- X86Simulator.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/SerDesUtils.h"
#include "snippy/Simulator/Targets/X86.h"

#include "Common.h"

#define DEBUG_TYPE "snippy-x86-sim"

namespace llvm {
namespace snippy {

void X86RegisterState::loadFromYamlFile(StringRef Filename,
                                        WarningsT &WarningsArr) {
  report_fatal_error("sorry not implemented");
}

void X86RegisterState::saveAsYAMLFile(raw_ostream &OS) const {
  report_fatal_error("sorry not implemented");
}

void X86RegisterState::randomize() {
  report_fatal_error("sorry not implemented");
}

bool X86RegisterState::operator==(const IRegisterState &) const {
  report_fatal_error("sorry not implemented");
}

std::unique_ptr<SimulatorInterface> createX86Simulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const TargetSubtargetInfo &Subtarget) {
  report_fatal_error("sorry not implemented");
}

} // namespace snippy
} // namespace llvm
