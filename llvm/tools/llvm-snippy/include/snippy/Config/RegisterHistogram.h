//===-- RegisterHistogram.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Valuegram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"

#include <random>
#include <string>
#include <variant>
#include <vector>

namespace llvm {
namespace snippy {

class SnippyTarget;

struct RegisterClassValues {
  std::string RegType;
  std::vector<APInt> Values;
};

struct RegisterValues {
  SmallVector<RegisterClassValues, 3> ClassValues;
};

struct RegisterClassHistogram {
  bool isEmpty() const { return TheValuegram.empty(); }
  std::string RegType;
  Valuegram TheValuegram;
};

struct RegisterHistograms {
  SmallVector<RegisterClassHistogram, 3> ClassHistograms;
};

struct AllRegisters final {
  static constexpr StringRef GPRPrefix = "X";
  static constexpr StringRef FPRPrefix = "F";
  static constexpr StringRef RVVPrefix = "V";
  static constexpr StringRef PCRegName = "PC";

  SmallVector<RegisterClassValues, 3> ClassValues;
  std::map<std::string, APInt> SpecialRegs;
  AllRegisters &addRegisterGroup(StringRef Prefix, ArrayRef<uint64_t> Values);

  AllRegisters &addRegisterGroup(StringRef Prefix, unsigned BitsNum,
                                 ArrayRef<APInt> Values);
  AllRegisters &addRegister(StringRef Name, APInt Value) {
    [[maybe_unused]] auto [It, Inserted] =
        SpecialRegs.try_emplace(Name.str(), Value);
    assert(Inserted);
    return *this;
  }

  void saveAsYAML(raw_ostream &OS);
};

struct RegistersWithHistograms {
  AllRegisters Registers;
  RegisterHistograms Histograms;
};

RegistersWithHistograms loadRegistersFromYaml(StringRef Path);

void checkRegisterClasses(const AllRegisters &Values,
                          ArrayRef<StringRef> AllowedClasses,
                          const SnippyTarget *Tgt = nullptr);

void checkRegisterClasses(const RegisterHistograms &Histograms,
                          ArrayRef<StringRef> AllowedClasses);

void getFixedRegisterValues(const RegistersWithHistograms &RH,
                            size_t ExpectedNumber, StringRef Prefix,
                            unsigned NumBits, std::vector<APInt> &Result);

APInt sampleValuegramForOneReg(const Valuegram &Valuegram, StringRef Prefix,
                               unsigned NumBits,
                               std::discrete_distribution<size_t> &Dist);

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<uint64_t> &Registers);

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<APInt> &Registers);

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::RegisterClassHistogram);

} // namespace llvm
