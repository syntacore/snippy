//===-- Common.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_LIB_SIMULATOR_COMMON_H
#define LLVM_TOOLS_LLVM_SNIPPY_LIB_SIMULATOR_COMMON_H

#include "snippy/Simulator/Simulator.h"

namespace llvm {
namespace snippy {
template <typename StateType, typename ControllerT, typename GPRType,
          typename FPRType>
class CommonSimulatorImpl : public SimulatorInterface {
protected:
  StateType ModelState;
  ~CommonSimulatorImpl() {}

public:
  CommonSimulatorImpl(StateType &&State) : ModelState(std::move(State)) {}

  ProgramCounterType readPC() const override { return ModelState.readPC(); }

  void logMessage(const Twine &Message) const override {
    ModelState.logMessage(Message.str().c_str());
  }

  const StateType &getLLImpl() const { return ModelState; }
  StateType &getLLImpl() { return ModelState; }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_LIB_SIMULATOR_COMMON_H
