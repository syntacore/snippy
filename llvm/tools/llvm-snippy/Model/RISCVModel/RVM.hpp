//===-- RVM.hpp -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "VTable.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

namespace rvm {

class State {
  struct StateDeleter {
    const RVM_FunctionPointers *VTable;

    void operator()(RVMState *State) const {
      if (State) {
        assert(VTable);
        VTable->modelDestroy(State);
      }
    }
  };

  std::unique_ptr<RVMState, StateDeleter> pimpl;

public:
  class Builder {
    RVMConfig Config = {};
    const RVM_FunctionPointers *VTable;

    std::string LogFilePath;
    std::string PluginInfo;

  public:
    Builder(const RVM_FunctionPointers *VTable) : VTable(VTable) {
      assert(VTable);
    }

    Builder &setRomStart(uint64_t Start) {
      Config.RomStart = Start;
      return *this;
    }

    Builder &setRomSize(uint64_t Size) {
      Config.RomSize = Size;
      return *this;
    }

    Builder &setRamStart(uint64_t Start) {
      Config.RamStart = Start;
      return *this;
    }

    Builder &setRamSize(uint64_t Size) {
      Config.RamSize = Size;
      return *this;
    }

    Builder &setRV64Isa() {
      Config.RV64 = 1;
      return *this;
    }

    Builder &setRV32Isa() {
      Config.RV64 = 0;
      return *this;
    }

    Builder &setMisa(uint64_t Misa) {
      Config.MisaExt = Misa;
      return *this;
    }

    Builder &registerCallbackHandler(RVMCallbackHandler *Handler) {
      Config.CallbackHandler = Handler;
      return *this;
    }

    Builder &registerMemUpdateCallback(MemUpdateCallbackTy Callback) {
      Config.MemUpdateCallback = Callback;
      return *this;
    }

    Builder &registerXRegUpdateCallback(XRegUpdateCallbackTy Callback) {
      Config.XRegUpdateCallback = Callback;
      return *this;
    }

    Builder &registerFRegUpdateCallback(FRegUpdateCallbackTy Callback) {
      Config.FRegUpdateCallback = Callback;
      return *this;
    }

    Builder &registerVRegUpdateCallback(VRegUpdateCallbackTy Callback) {
      Config.VRegUpdateCallback = Callback;
      return *this;
    }

    Builder &registerPCUpdateCallback(PCUpdateCallbackTy Callback) {
      Config.PCUpdateCallback = Callback;
      return *this;
    }

    Builder &setZext(uint64_t Zext) {
      Config.ZExt = Zext;
      return *this;
    }

    Builder &setXext(uint64_t Xext) {
      Config.XExt = Xext;
      return *this;
    }

    Builder &setVLEN(unsigned VLEN) {
      Config.VLEN = VLEN;
      return *this;
    }

    Builder &enableMisalignedAccess() {
      Config.EnableMisalignedAccess = true;
      return *this;
    }

    Builder &setLogPath(std::string LogFilePathIn) {
      LogFilePath = std::move(LogFilePathIn);
      Config.LogFilePath = LogFilePath.c_str();
      return *this;
    }

    Builder &setPluginInfo(std::string PluginInfoIn) {
      PluginInfo = std::move(PluginInfoIn);
      Config.PluginInfo = PluginInfo.c_str();
      return *this;
    }

    Builder &changeMaskAgnosticElems() {
      Config.ChangeMaskAgnosticElems = true;
      return *this;
    }

    Builder &changeTailAgnosticElems() {
      Config.ChangeTailAgnosticElems = true;
      return *this;
    }

    State build() { return {VTable, &Config}; }
  };

  State(const RVM_FunctionPointers *VTable, const RVMConfig *Config)
      : pimpl(VTable->modelCreate(Config), StateDeleter{VTable}) {
    assert(VTable);
  }

  RVMState *get() { return pimpl.get(); }
  const RVMState *get() const { return pimpl.get(); }

  const RVM_FunctionPointers *getVTable() const {
    return pimpl.get_deleter().VTable;
  }

  const RVMConfig &getConfig() const {
    const auto *Config = getVTable()->getModelConfig(get());
    assert(Config);
    return *Config;
  }

  int executeInstr() { return getVTable()->executeInstr(get()); }

  template <typename T>
  void readMem(uint64_t Addr, size_t Count, T *Data) const {
    getVTable()->readMem(get(), Addr, Count * sizeof(T),
                         reinterpret_cast<char *>(Data));
  }

  template <typename T>
  void writeMem(uint64_t Addr, size_t Count, const T *Data) {
    getVTable()->writeMem(get(), Addr, Count * sizeof(T),
                          reinterpret_cast<const char *>(Data));
  }

  uint64_t readPC() const { return getVTable()->readPC(get()); }
  void setPC(uint64_t NewPC) { getVTable()->setPC(get(), NewPC); }

  RVMRegT readXReg(RVMXReg Reg) const {
    return getVTable()->readXReg(get(), Reg);
  }

  void setXReg(RVMXReg Reg, RVMRegT Value) {
    return getVTable()->setXReg(get(), Reg, Value);
  }

  RVMRegT readFReg(RVMFReg Reg) const {
    return getVTable()->readFReg(get(), Reg);
  }

  void setFReg(RVMFReg Reg, RVMRegT Value) {
    return getVTable()->setFReg(get(), Reg, Value);
  }

  RVMRegT readCSRReg(unsigned Reg) const {
    return getVTable()->readCSRReg(get(), Reg);
  }

  void setCSRReg(unsigned Reg, RVMRegT Value) {
    return getVTable()->setCSRReg(get(), Reg, Value);
  }

  int readVReg(RVMVReg Reg, char *Data, size_t MaxSize) const {
    return getVTable()->readVReg(get(), Reg, Data, MaxSize);
  }

  int setVReg(RVMVReg Reg, const char *Data, size_t DataSize) {
    return getVTable()->setVReg(get(), Reg, Data, DataSize);
  }

  void logMessage(const char *Message) const {
    return getVTable()->logMessage(Message);
  }
};

} // namespace rvm
