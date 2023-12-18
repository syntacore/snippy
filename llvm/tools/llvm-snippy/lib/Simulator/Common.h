//===-- Common.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

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
  void setPC(ProgramCounterType NewPC) override { ModelState.setPC(NewPC); }

  RegisterType readGPR(unsigned RegID) const override {
    return ModelState.readXReg(static_cast<GPRType>(RegID));
  }

  virtual void setGPR(unsigned RegID, RegisterType NewValue) override {
    ModelState.setXReg(static_cast<GPRType>(RegID), NewValue);
  }

  RegisterType readFPR(unsigned RegID) const override {
    return ModelState.readFReg(static_cast<FPRType>(RegID));
  }
  void setFPR(unsigned RegID, RegisterType NewValue) override {
    ModelState.setFReg(static_cast<FPRType>(RegID), NewValue);
  }

  void readMem(MemoryAddressType Addr,
               MutableArrayRef<char> Data) const override {
    ModelState.readMem(Addr, Data.size(), Data.data());
  }

  void writeMem(MemoryAddressType Addr, ArrayRef<char> Data) override {
    ModelState.writeMem(Addr, Data.size(), Data.data());
  }

  void logMessage(const Twine &Message) const override {
    ModelState.logMessage(Message.str().c_str());
  }

  const StateType &getLLImpl() const { return ModelState; }
  StateType &getLLImpl() { return ModelState; }
};

} // namespace snippy
} // namespace llvm
