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
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

/** @file RVM.hpp This file defines C++ interface for RVM
 *
 * It builds on existing functions and classes declared in @ref RVM.h
 */

/** @brief namespace containing all RVM interface functions */
namespace rvm {
/// @cond
namespace detail {
// replace composite extensions with their components
inline RVMExtDescriptor normalize_extensions(const RVMExtDescriptor &Ext,
                                             bool RV64 = true) {
  RVMExtDescriptor Norm;
  Norm.XExtSize = Ext.XExtSize;
  Norm.ZExtSize = Ext.ZExtSize;
  std::copy(std::begin(Ext.ZExt), std::end(Ext.ZExt), Norm.ZExt);
  std::copy(std::begin(Ext.XExt), std::end(Ext.XExt), Norm.XExt);
  std::copy(std::begin(Ext.MisaExt), std::end(Ext.MisaExt), Norm.MisaExt);
  auto &ZExt = Norm.ZExt;

  // MISA extensions
  auto &MisaExt = Norm.MisaExt;
  // I" shall be selected over "E" if both are available. -
  // privileged(20260120) 3.1.1
  if (MisaExt[RVM_MISA_I] && MisaExt[RVM_MISA_E])
    MisaExt[RVM_MISA_E] = 0;

  if (MisaExt[RVM_MISA_G]) {
    MisaExt[RVM_MISA_I] = true;
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
  // Zc* v1.0.4-2 spec 1.5
  // MISA.C is set if the following extensions are selected:
  // - Zca and not F
  if (ZExt[RVM_ZEXT_CA] && !MisaExt[RVM_MISA_F]) {
    MisaExt[RVM_MISA_C] = true;
  }
  // - Zca, Zcf and F is specified (RV32 only)
  if (ZExt[RVM_ZEXT_CA] && ZExt[RVM_ZEXT_CF] && MisaExt[RVM_MISA_F] && !RV64) {
    MisaExt[RVM_MISA_C] = true;
  }
  // - Zca, Zcf and Zcd if D is specified (RV32 only)
  if (ZExt[RVM_ZEXT_CA] && ZExt[RVM_ZEXT_CF] && ZExt[RVM_ZEXT_CD] &&
      MisaExt[RVM_MISA_D] && !RV64) {
    MisaExt[RVM_MISA_C] = true;
  }
  // - Zca, Zcd if D is specified (RV64 only)
  if (ZExt[RVM_ZEXT_CA] && ZExt[RVM_ZEXT_CD] && MisaExt[RVM_MISA_D] && RV64) {
    MisaExt[RVM_MISA_C] = true;
  }
  return Norm;
}
/// @cond
#ifdef RVM_ADD_MISA_STRING_CASE
#error RVM_ADD_MISA_STRING_CASE should not be defined at this point
#else
#define RVM_ADD_MISA_STRING_CASE(Name, name)                                   \
  if (MisaExt[Name])                                                           \
    OS << #name;
#endif
/// @endcond
inline void add_misa(std::ostream &OS, const RVMExtDescriptor &Ext) {
  auto &MisaExt = Ext.MisaExt;
  RVM_FOR_EACH_MISA_EXT(RVM_ADD_MISA_STRING_CASE)
}
#undef RVM_ADD_MISA_STRING_CASE

/// @cond
#ifdef RVM_RVM_ADD_ZEXT_BITS_STRING_CASE
#error RVM_RVM_ADD_ZEXT_BITS_STRING_CASE should not be defined at this point
#else
#define RVM_RVM_ADD_ZEXT_BITS_STRING_CASE(NAME, name)                          \
  if (ZExt[NAME])                                                              \
    OS << "_Z" << #name;
#endif
/// @endcond
inline void add_standard_extensions(std::ostream &OS,
                                    const RVMExtDescriptor &Ext) {
  auto &ZExt = Ext.ZExt;
  RVM_FOR_EACH_ZEXT(RVM_RVM_ADD_ZEXT_BITS_STRING_CASE);
}
#undef RVM_RVM_ADD_ZEXT_BITS_STRING_CASE

/// @cond
#ifdef RVM_RVM_ADD_XEXT_BITS_STRING_CASE
#error RVM_RVM_ADD_XEXT_BITS_STRING_CASE should not be defined at this point
#else
#define RVM_RVM_ADD_XEXT_BITS_STRING_CASE(NAME, name)                          \
  if (XExt[NAME])                                                              \
    OS << "_X" << #name;
#endif
/// @endcond
inline void add_custom_extensions(std::ostream &OS,
                                  const RVMExtDescriptor &Ext) {
  auto &XExt = Ext.XExt;
  RVM_FOR_EACH_XEXT(RVM_RVM_ADD_XEXT_BITS_STRING_CASE);
}
#undef RVM_RVM_ADD_XEXT_BITS_STRING_CASE

} // namespace detail

///@endcond

/** @brief Creates valid ISA string from given extension descriptor
 *
 * @param Ext Extension descriptor to create ISA string from
 * @param RV64 Whether we are dealing with 64 or 32-bit
 * @param Lowercase If true ISA string will be lowercase (Defaults to false)
 * @returns Valid ISA string
 */
inline std::string create_isa_string(const RVMExtDescriptor &Ext, bool RV64,
                                     bool Lowercase = false) {
  assert(sizeof(Ext.ZExt) == Ext.ZExtSize);
  assert(sizeof(Ext.XExt) == Ext.XExtSize);
  auto Norm = detail::normalize_extensions(Ext, RV64);
  std::stringstream SS;
  SS << (RV64 ? "RV64" : "RV32");
  detail::add_misa(SS, Norm);
  detail::add_standard_extensions(SS, Norm);
  detail::add_custom_extensions(SS, Norm);
  auto Isa = SS.str();
  auto FirstUnderscore = std::find(Isa.begin(), Isa.end(), '_');
  // capitalize MISA extensions
  std::transform(Isa.begin(), FirstUnderscore, Isa.begin(),
                 [](auto C) { return std::toupper(C); });
  if (FirstUnderscore != Isa.end())
    Isa.erase(FirstUnderscore);
  if (Lowercase) {
    std::transform(Isa.begin(), Isa.end(), Isa.begin(),
                   [](auto C) { return std::tolower(C); });
  }
  return Isa;
}

/** @brief Main interface class */
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
  /** @brief Builder for rvm::State
   *
   * All methods of this class essentially create internal instance of @ref
   * RVMConfig and then pass it to @ref State constructor
   */
  class Builder {
    RVMConfig Config = {};
    const RVM_FunctionPointers *VTable;

    std::vector<RVMMemoryRegion> MemoryRegions;
    // NOTE: Use std::list because we need pointer stability for .c_str().
    std::list<std::string> MemoryRegionNames;
    std::optional<std::string> LogFilePath;
    std::optional<std::string> DebugLogFilePath;
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

    /** @brief Creates memory region
     *
     *  Adds it to the internal array that will be used in @ref RVMConfig
     * */
    Builder &addMemoryRegion(uint64_t Start, uint64_t Size, const char *Name) {
      RVMMemoryRegion Region{Start, Size, nullptr};
      if (Name) {
        MemoryRegionNames.emplace_back(Name);
        auto &NameStr = MemoryRegionNames.back();
        Region.Name = NameStr.c_str();
      }
      MemoryRegions.push_back(Region);
      Config.MemoryRegions = &MemoryRegions.front();
      Config.MemoryRegionCount = MemoryRegions.size();
      return *this;
    }

    /** @brief Set this to use rv64 model*/
    Builder &setRV64Isa() {
      Config.RV64 = 1;
      return *this;
    }

    /** @brief Set this to use rv32 model*/
    Builder &setRV32Isa() {
      Config.RV64 = 0;
      return *this;
    }

    Builder &registerCallbackHandler(RVMCallbackHandler *Handler) {
      Config.CallbackHandler = Handler;
      return *this;
    }

    Builder &registerMemReadCallback(MemReadCallbackTy Callback) {
      Config.MemReadCallback = Callback;
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

    Builder &registerCSRUpdateCallback(CSRUpdateCallbackTy Callback) {
      Config.CSRUpdateCallback = Callback;
      return *this;
    }

    Builder &registerPCUpdateCallback(PCUpdateCallbackTy Callback) {
      Config.PCUpdateCallback = Callback;
      return *this;
    }

    /** @brief copies extension descriptor to @ref RVMConfig */
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

    /** @brief Sets VLEN value to be used by vector instructions */
    Builder &setVLEN(unsigned VLEN) {
      Config.VLEN = VLEN;
      return *this;
    }

    /** @brief Allows misaligned accesses
     *
     * Works the same as specifying zicclsm but this extension is not supported
     * yet
     *
     * FIXME: remove this when adding support for Zicclsm
     *
     */
    Builder &enableMisalignedAccess() {
      Config.EnableMisalignedAccess = true;
      return *this;
    }
    /** @brief sets file path for the execution logs
     *
     * empty - stdout, "-" for stderr.
     */
    Builder &setLogPath(std::string LogFilePathIn) {
      LogFilePath = std::move(LogFilePathIn);
      Config.LogFilePath = LogFilePath->c_str();
      return *this;
    }

    /** @brief Disable execution logs */
    Builder &disableLogs() {
      LogFilePath = std::nullopt;
      Config.LogFilePath = nullptr;
      return *this;
    }
    /** @brief sets file path for the debug logs
     *
     * Empty - stdout, "-" for stderr.
     */

    Builder &setDebugLogPath(std::string LogFilePathIn) {
      DebugLogFilePath = std::move(LogFilePathIn);
      Config.DebugLogFilePath = DebugLogFilePath->c_str();
      return *this;
    }

    /** @brief Disable debug logs */
    Builder &disableDebugLogs() {
      DebugLogFilePath = std::nullopt;
      Config.DebugLogFilePath = nullptr;
      return *this;
    }

    /** @brief Whether or not to set mask agnostic bits in vector operations
     *
     * Agnostic bits are undisturbed by-default
     *
     */
    Builder &changeMaskAgnosticElems() {
      Config.ChangeMaskAgnosticElems = true;
      return *this;
    }

    /** @brief Whether or not to set tail agnostic bits in vector operations
     *
     * Agnostic bits are undisturbed by-default
     *
     */
    Builder &changeTailAgnosticElems() {
      Config.ChangeTailAgnosticElems = true;
      return *this;
    }

    /** @brief Actually builds @ref State with created config */
    std::variant<State, std::string> build() {
      RVMErrorCode Err = RVM_ERRC_SUCCESS;
      const unsigned ErrBufSize = 200u;
      std::string ErrStr(ErrBufSize, '\0');
      auto *ModelPtr =
          VTable->modelCreate(&Config, &Err, ErrStr.data(), ErrBufSize);
      if (Err != RVM_ERRC_SUCCESS)
        return std::string("Failed to create RVMState: ") + ErrStr;
      return State(VTable, ModelPtr);
    }
  };

private:
  State(const RVM_FunctionPointers *VTable, RVMState *StatePtr)
      : pimpl(StatePtr, StateDeleter{VTable}) {
    assert(VTable);
  }

public:
  static std::string strerror(RVMErrorCode Err) { return rvm_strerror(Err); }

  /**
   * @brief Returns textual explanation of the last error
   *
   * The returned string is implementation-defined and should contain
   * additional information that helps diagnose the error.
   *
   * For example, if @ref readMem returns
   * @ref RVM_ERRC_INVALID_ADDRESS, the returned message may contain
   * the invalid address and the ranges of available memory.
   *
   * @returns Error context string. Empty string means that no additional
   *          context is available.
   */
  std::string getErrorContext() const {
    size_t Size = 0;
    getVTable()->getErrorContext(get(), nullptr, &Size);
    if (Size == 0)
      return {};
    std::string Result(Size, '\0');
    getVTable()->getErrorContext(get(), Result.data(), &Size);
    // In case implementation changed the size between calls.
    Result.resize(std::min(Result.size(), Size));
    return Result;
  }

  /** @brief Returns pointer to internal @ref RVMState implementation */
  RVMState *get() { return pimpl.get(); }
  /** @brief Returns pointer to internal @ref RVMState implementation */
  const RVMState *get() const { return pimpl.get(); }

  /** @brief Returns pointer to internal VTable instance */
  const RVM_FunctionPointers *getVTable() const {
    return pimpl.get_deleter().VTable;
  }

  /** @brief Returns reference to internal Config */
  const RVMConfig &getConfig() const {
    const auto *Config = getVTable()->getModelConfig(get());
    assert(Config);
    return *Config;
  }
  /**
   * @brief Resets model to the initial state
   *
   * After reset the model is in exact state as model that was just created from
   * the same config
   */
  void reset() { getVTable()->modelReset(get()); }

  /** @brief Executes single instruction under current PC register */
  RVMSimExecStatus executeInstr() { return getVTable()->executeInstr(get()); }

  /**
   * @brief Reads data from memory
   *
   * @param Addr Memory location to read from
   *
   * @param Count Number of elements of type T to copy
   *
   * @param[out] Data Pointer to the memory location to copy to. Should have at
   * least Count * sizeof(T) bytes accessible
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ADDRESS if attempted to access memory that was not
   * allocated by any @ref RVMMemoryRegion
   */
  template <typename T>
  RVM_NODISCARD RVMErrorCode readMem(uint64_t Addr, size_t Count,
                                     T *Data) const {
    return getVTable()->readMem(get(), Addr, Count * sizeof(T),
                                reinterpret_cast<char *>(Data));
  }
  /**
   * @brief Writes to memory
   *
   * @param Addr Model's memory location to write to
   * @param Count Number of elements to copy
   * @param Data Pointer to the memory location to copy Count elements of type T
   * from
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ADDRESS if attempted to access memory that was not
   * allocated by any @ref RVMMemoryRegion
   */
  template <typename T>
  RVM_NODISCARD RVMErrorCode writeMem(uint64_t Addr, size_t Count,
                                      const T *Data) {
    return getVTable()->writeMem(get(), Addr, Count * sizeof(T),
                                 reinterpret_cast<const char *>(Data));
  }
  /**
   * @brief sets model's stop mode
   *
   * @param Mode stop mode
   */
  void setStopMode(RVMStopMode Mode) { getVTable()->setStopMode(get(), Mode); }
  /**
   * @brief sets stop PC
   *
   * @param Addr PC value to stop execution at. Has no effect if StopMode !=
   * @ref RVM_STOP_BY_PC
   *
   * @return @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ADDRESS if Addr is wider than PC register
   */
  RVM_NODISCARD
  RVMErrorCode setStopPC(uint64_t StopPC) {
    return getVTable()->setStopPC(get(), StopPC);
  }
  /**
   * @brief Reads current PC
   *
   * @returns Current value of PC register
   */
  uint64_t readPC() const { return getVTable()->readPC(get()); }
  /**
   * @brief Sets PC register
   *
   * @param NewPC Value to assign to PC register
   *
   * @return @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ADDRESS if NewPC is wider than PC register
   */
  RVM_NODISCARD
  RVMErrorCode setPC(uint64_t NewPC) {
    return getVTable()->setPC(get(), NewPC);
  }
  /**
   * @brief Reads GPR value
   *
   * Value written to Val is zero-extended to 64 bit
   *
   * @param Reg Register to read
   * @param[out] Val Pointer to a variable to write register value to. Untouched
   * on error.
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode readXReg(RVMXReg Reg, RVMRegT &Val) const {
    return getVTable()->readXReg(get(), Reg, &Val);
  }
  /**
   * @brief Sets GPR to value
   *
   * @param Reg Register to set
   * @param Value Value that will be written to Reg
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ARGUMENT if Value is wider than register.
   * @ref RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode setXReg(RVMXReg Reg, RVMRegT Value) {
    return getVTable()->setXReg(get(), Reg, Value);
  }
  /**
   * @brief Reads FPR value
   *
   * Value written to Val is zero-extended to 64-bit
   *
   * @param Reg Register to read
   * @param[out] Val Pointer to a variable to write register value to. Untouched
   * on error.
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode readFReg(RVMFReg Reg, RVMRegT &Val) const {
    return getVTable()->readFReg(get(), Reg, &Val);
  }
  /**
   * @brief Sets FPR to value
   *
   * @param Reg Register to set
   * @param Value Value to write to register denoted by Reg
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ARGUMENT if Value is wider than register.
   * @ref RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode setFReg(RVMFReg Reg, RVMRegT Value) {
    return getVTable()->setFReg(get(), Reg, Value);
  }
  /**
   * @brief Reads CSR value
   *
   * Value written to Val is zero-extended to 64-bit
   *
   * @param Reg Register to read
   * @param[out] Val Pointer to a variable to write register value to. Untouched
   * on error.
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ADDRESS if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode readCSR(unsigned Reg, RVMRegT &Val) const {
    return getVTable()->readCSR(get(), Reg, &Val);
  }
  /**
   * @brief Sets CSR to value
   *
   * @param Reg Register to set
   * @param Value Value to write to register denoted by Reg
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ARGUMENT if Value is wider than register.
   * @ref RVM_ERRC_IDX_OUT_OF_RANGE if unsupported Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode setCSR(unsigned Reg, RVMRegT Value) {
    return getVTable()->setCSR(get(), Reg, Value);
  }
  /**
   * @brief Raises external interrupt
   *
   * @param Value MCAUSE will be set to this value
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ARGUMENT if Cause is wider than MCAUSE register.
   */

  RVM_NODISCARD
  RVMErrorCode raiseInterrupt(RVMRegT Cause) {
    return getVTable()->raiseInterrupt(get(), Cause);
  }
  /**
   * @brief Clears interrupt status and sets MCAUSE CSR
   *
   * @param Value MCAUSE will be set to this value
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_INVALID_ARGUMENT if Cause is wider than MCAUSE register.
   */
  RVM_NODISCARD
  RVMErrorCode clearInterrupt(RVMRegT Cause) {
    return getVTable()->clearInterrupt(get(), Cause);
  }

  /**
   * @brief Reads vector register
   *
   * @param Reg Vector register to read
   * @param[out] Data Pointer to buffer to copy vector to
   *
   * @param[in,out] MaxSize Maximal vector register size (in bytes). Serves as a
   * limiter to avoid overflow. If @p Data is NULL then required size will be
   * written to @p MaxSize. If @p MaxSize can't fit target register only @p
   * MaxSize bits will be copied and necessary register size will be written to
   * @p MaxSize.
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported @p Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode readVReg(RVMVReg Reg, char *Data, size_t &MaxSize) const {
    return getVTable()->readVReg(get(), Reg, Data, &MaxSize);
  }
  /**
   * @brief Writes value to vector register
   *
   * @param Reg Vector register to update
   *
   * @param Data Pointer to read new vector register value from. Should have at
   * least VLENB bytes.
   *
   * @param MaxSize Maximal vector register size (in bytes). Serves as a limiter
   * to avoid overflow. If @p Data is NULL then required size will be written to
   * @p MaxSize. If @p MaxSize can't fit target register only @p MaxSize bits
   * will be copied and necessary register size will be written to @p MaxSize.
   *
   * @returns @ref RVM_ERRC_SUCCESS if no errors occurred. @ref
   * RVM_ERRC_IDX_OUT_OF_RANGE if unsupported @p Reg was specified
   */
  RVM_NODISCARD
  RVMErrorCode setVReg(RVMVReg Reg, const char *Data, size_t &DataSize) {
    return getVTable()->setVReg(get(), Reg, Data, &DataSize);
  }
  /**
   * @brief Appends custom message to model logs
   *
   * @param Message '\0'-terminated string to append
   */
  void logMessage(const char *Message) const {
    return getVTable()->logMessage(get(), Message);
  }
};

} // namespace rvm

