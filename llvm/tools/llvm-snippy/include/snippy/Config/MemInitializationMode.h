//===-- MemInitializationMode.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLUtils.h"
#include <algorithm>
#include <array>

namespace llvm {
namespace snippy {

struct MemInitMode {
  enum class Mode {
    NoInit,
    Full,
    RuntimeFull,
    LoadsOnly,
    FullWithAddresses,
    LoadsWithAddresses,
    TextFileInit
  };

#define SNIPPY_MEM_INIT_DECL_ENUM_VAL_(Val)                                    \
  static constexpr auto Val = Mode::Val
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(NoInit);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(Full);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(RuntimeFull);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(LoadsOnly);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(FullWithAddresses);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(LoadsWithAddresses);
  SNIPPY_MEM_INIT_DECL_ENUM_VAL_(TextFileInit);
#undef SNIPPY_MEM_INIT_DECL_ENUM_VAL_

  constexpr MemInitMode(Mode V = Mode::NoInit) : Value(V) {}
  explicit constexpr MemInitMode(std::underlying_type_t<Mode> V)
      : Value(static_cast<Mode>(V)) {}

  constexpr operator bool() const { return Value == Mode::NoInit; }

  constexpr bool operator==(const MemInitMode &R) const {
    return Value == R.Value;
  }

  constexpr bool operator!=(const MemInitMode &R) const {
    return !(*this == R);
  }

  static constexpr auto RuntimeInitModes =
      std::array{Mode::LoadsOnly, Mode::RuntimeFull, Mode::LoadsWithAddresses};

  static constexpr auto InitModesWithOptionalSeed =
      std::array{Mode::Full, Mode::LoadsOnly, Mode::FullWithAddresses,
                 Mode::LoadsWithAddresses};

  static constexpr auto LoadsInitModes =
      std::array{Mode::LoadsOnly, Mode::LoadsWithAddresses};

  static constexpr auto InitWithoutSeed = std::array{Mode::TextFileInit};

  static constexpr auto InitFromFile = std::array{Mode::TextFileInit};

  bool isDuringRuntime() const {
    return std::count(RuntimeInitModes.begin(), RuntimeInitModes.end(), Value);
  }

  friend bool isDuringRuntime(const MemInitMode &MIM) {
    return MIM.isDuringRuntime();
  }

  bool isSeedOptional() const {
    return std::count(InitModesWithOptionalSeed.begin(),
                      InitModesWithOptionalSeed.end(), Value);
  }

  friend bool isSeedOptional(const MemInitMode &MIM) {
    return MIM.isSeedOptional();
  }

  bool isLoadsInit() const {
    return std::count(LoadsInitModes.begin(), LoadsInitModes.end(), Value);
  }

  friend bool isLoadsInit(const MemInitMode &MIM) { return MIM.isLoadsInit(); }

  bool isSeedProhibited() const {
    return std::count(InitWithoutSeed.begin(), InitWithoutSeed.end(), Value);
  }

  friend bool isSeedProhibited(const MemInitMode &MIM) {
    return MIM.isSeedProhibited();
  }

  bool isFileInit() const {
    return std::count(InitFromFile.begin(), InitFromFile.end(), Value);
  }

  friend bool isFileInit(const MemInitMode &MIM) { return MIM.isFileInit(); }

  Mode Value = Mode::NoInit;
};

struct MemInitModeEnumOption
    : public snippy::EnumOptionMixin<MemInitModeEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(MemInitMode::NoInit, "no",
                    "Generation without memory initialization");
    Mapper.enumCase(MemInitMode::RuntimeFull, "runtime",
                    "All RW sections will be initialized during "
                    "runtime with function using SplitMix32 algorithm");
    Mapper.enumCase(MemInitMode::Full, "full",
                    "All RW sections will be filled"
                    " with randomized values"
                    "(depending on instructions seed)");
    Mapper.enumCase(MemInitMode::LoadsOnly, "loads",
                    "Initialization during runtime "
                    "of the addresses "
                    "where load will be performed");
    Mapper.enumCase(MemInitMode::FullWithAddresses, "full-with-addresses",
                    "All RW sections will be filled"
                    " with addresses pointing to valid memory in RW sections"
                    "(depending on instructions seed and memory scheme)");
    Mapper.enumCase(MemInitMode::LoadsWithAddresses, "loads-with-addresses",
                    "Initialization during runtime "
                    "of the addresses "
                    "where load will be performed with values that are valid "
                    "addresses (specified by the memory scheme)");
    Mapper.enumCase(MemInitMode::TextFileInit, "ascii-file",
                    "Specifies the initial memory state with text file. "
                    "File format is equivalent to the one that Snippy produces "
                    "with --dump-memory-as-ascii option.");
  }
};

} // namespace snippy

template <>
struct cl::OptionValue<snippy::MemInitMode> final
    : cl::OptionValueCopy<snippy::MemInitMode> {
  using WrapperType = snippy::MemInitMode;

  OptionValue() = default;

  OptionValue(const snippy::MemInitMode &V) { this->setValue(V); }

  OptionValue<snippy::MemInitMode> &operator=(const snippy::MemInitMode &V) {
    setValue(V);
    return *this;
  }

private:
  void anchor() override {}
};

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::MemInitMode);

} // namespace llvm
