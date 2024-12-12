//===-- RVM.hpp -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "VTable.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace rvm {
namespace detail {
// replace composite extensions with their components
inline RVMExtDescriptor normalize_extensions(const RVMExtDescriptor &Ext) {
  RVMExtDescriptor Norm;
  std::copy(std::begin(Ext.ZExt), std::end(Ext.ZExt), Norm.ZExt);
  std::copy(std::begin(Ext.XExt), std::end(Ext.XExt), Norm.XExt);
  std::copy(std::begin(Ext.MisaExt), std::end(Ext.MisaExt), Norm.MisaExt);
  auto &ZExt = Norm.ZExt;
  // MISA extensions
  auto &MisaExt = Norm.MisaExt;
  if (MisaExt[RVM_MISA_G]) {
    MisaExt[RVM_MISA_M] = true;
    MisaExt[RVM_MISA_A] = true;
    MisaExt[RVM_MISA_F] = true;
    MisaExt[RVM_MISA_D] = true;
    ZExt[RVM_ZEXT_IFENCEI] = true;
    ZExt[RVM_ZEXT_ICSR] = true;
    MisaExt[RVM_MISA_G] = false;
  }
  // standard extensions
  if (ZExt[RVM_ZEXT_KN]) {
    ZExt[RVM_ZEXT_BKB] = true;
    ZExt[RVM_ZEXT_BKC] = true;
    ZExt[RVM_ZEXT_BKX] = true;
    ZExt[RVM_ZEXT_KNE] = true;
    ZExt[RVM_ZEXT_KND] = true;
    ZExt[RVM_ZEXT_KNH] = true;
    ZExt[RVM_ZEXT_KN] = false;
  }
  if (ZExt[RVM_ZEXT_KS]) {
    ZExt[RVM_ZEXT_BKB] = true;
    ZExt[RVM_ZEXT_BKC] = true;
    ZExt[RVM_ZEXT_BKX] = true;
    ZExt[RVM_ZEXT_KSED] = true;
    ZExt[RVM_ZEXT_KSH] = true;
    ZExt[RVM_ZEXT_KS] = false;
  }
  if (ZExt[RVM_ZEXT_K]) {
    ZExt[RVM_ZEXT_KN] = true;
    ZExt[RVM_ZEXT_KR] = true;
    ZExt[RVM_ZEXT_KT] = true;
    ZExt[RVM_ZEXT_K] = false;
  }
  if (ZExt[RVM_ZEXT_BITMANIP]) {
    ZExt[RVM_ZEXT_BA] = true;
    ZExt[RVM_ZEXT_BB] = true;
    ZExt[RVM_ZEXT_BC] = true;
    ZExt[RVM_ZEXT_BS] = true;
    ZExt[RVM_ZEXT_BITMANIP] = false;
  }
  if (ZExt[RVM_ZEXT_VKN]) {
    ZExt[RVM_ZEXT_VKNED] = true;
    ZExt[RVM_ZEXT_VKNHB] = true;
    ZExt[RVM_ZEXT_VKB] = true;
    ZExt[RVM_ZEXT_VKT] = true;
    ZExt[RVM_ZEXT_VKN] = false;
  }
  if (ZExt[RVM_ZEXT_VKNC]) {
    ZExt[RVM_ZEXT_VKN] = true;
    ZExt[RVM_ZEXT_VBC] = true;
    ZExt[RVM_ZEXT_VKNC] = false;
  }
  if (ZExt[RVM_ZEXT_VKNG]) {
    ZExt[RVM_ZEXT_VKN] = true;
    ZExt[RVM_ZEXT_VKG] = true;
    ZExt[RVM_ZEXT_VKNG] = false;
  }
  if (ZExt[RVM_ZEXT_VKS]) {
    ZExt[RVM_ZEXT_VKSED] = true;
    ZExt[RVM_ZEXT_VKSH] = true;
    ZExt[RVM_ZEXT_VKB] = true;
    ZExt[RVM_ZEXT_VKT] = true;
    ZExt[RVM_ZEXT_VKS] = false;
  }
  if (ZExt[RVM_ZEXT_VKSC]) {
    ZExt[RVM_ZEXT_VKS] = true;
    ZExt[RVM_ZEXT_VBC] = true;
    ZExt[RVM_ZEXT_VKSC] = false;
  }
  if (ZExt[RVM_ZEXT_VKSG]) {
    ZExt[RVM_ZEXT_VKS] = true;
    ZExt[RVM_ZEXT_VKG] = true;
    ZExt[RVM_ZEXT_VKSG] = false;
  }
  return Norm;
}
#ifdef ADD_MISA_CASE
#error ADD_MISA_CASE should not be defined at this point
#else
#define ADD_MISA_CASE(Name, name)                                              \
  if (MisaExt[Name])                                                           \
    OS << #name;
#endif
inline void add_misa(std::ostream &OS, const RVMExtDescriptor &Ext) {
  auto &MisaExt = Ext.MisaExt;
  RVM_FOR_EACH_MISA_EXT(ADD_MISA_CASE)
}
#undef ADD_MISA_CASE

#ifdef ADD_ZEXT_CASE
#error ADD_ZEXT_CASE should not be defined at this point
#else
#define ADD_ZEXT_CASE(NAME, name)                                              \
  if (ZExt[NAME])                                                              \
    OS << "_Z" << #name;
#endif
inline void add_standard_extensions(std::ostream &OS,
                                    const RVMExtDescriptor &Ext) {
  auto &ZExt = Ext.ZExt;
  RVM_FOR_EACH_ZEXT(ADD_ZEXT_CASE);
}
#undef ADD_ZEXT_CASE

#ifdef ADD_XEXT_CASE
#error ADD_XEXT_CASE should not be defined at this point
#else
#define ADD_XEXT_CASE(NAME, name)                                              \
  if (XExt[NAME])                                                              \
    OS << "_X" << #name;
#endif
inline void add_custom_extensions(std::ostream &OS,
                                  const RVMExtDescriptor &Ext) {
  auto &XExt = Ext.XExt;
  RVM_FOR_EACH_XEXT(ADD_XEXT_CASE);
}
#undef ADD_XEXT_CASE

} // namespace detail

inline std::string create_isa_string(const RVMExtDescriptor &Ext, bool RV64,
                                     bool Lowercase = false) {
  assert(sizeof(Ext.ZExt) == Ext.ZExtSize);
  assert(sizeof(Ext.XExt) == Ext.XExtSize);
  auto Norm = detail::normalize_extensions(Ext);
  std::stringstream SS;
  SS << (RV64 ? "RV64I" : "RV32I");
  detail::add_misa(SS, Norm);
  detail::add_standard_extensions(SS, Norm);
  detail::add_custom_extensions(SS, Norm);
  auto Isa = SS.str();
  auto FirstUnderscore = std::find(Isa.begin(), Isa.end(), '_');
  if (FirstUnderscore != Isa.end())
    Isa.erase(FirstUnderscore);
  if (Lowercase) {
    std::string LoweredIsa;
    std::transform(Isa.begin(), Isa.end(), std::back_inserter(LoweredIsa),
                   [](auto C) { return std::tolower(C); });
    return LoweredIsa;
  }

  return Isa;
}

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

    Builder(const Builder &) = delete;
    Builder(Builder &&OldBuild) = default;
    Builder &operator=(const Builder &) = delete;
    Builder &operator=(Builder &&OldBuild) = default;
    ~Builder() = default;

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

    Builder &setExtensions(const RVMExtDescriptor &Ext) {
      std::copy(std::begin(Ext.ZExt), std::end(Ext.ZExt),
                Config.Extensions.ZExt);
      std::copy(std::begin(Ext.XExt), std::end(Ext.XExt),
                Config.Extensions.XExt);
      std::copy(std::begin(Ext.MisaExt), std::end(Ext.MisaExt),
                Config.Extensions.MisaExt);
      Config.Extensions.ZExtSize = Ext.ZExtSize;
      Config.Extensions.XExtSize = Ext.XExtSize;
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

  RVMSimExecStatus executeInstr() { return getVTable()->executeInstr(get()); }

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

  void setStopMode(RVMStopMode Mode) { getVTable()->setStopMode(get(), Mode); }
  void setStopPC(uint64_t StopPC) { getVTable()->setStopPC(get(), StopPC); }

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
