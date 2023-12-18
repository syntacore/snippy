//===-- RegisterHistogram.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace llvm::snippy {

struct RegisterClassValues {
  std::string RegType;
  std::vector<APInt> Values;
};

struct RegisterValues {
  SmallVector<RegisterClassValues, 3> ClassValues;
};

struct RegisterClassHistogram {
  enum class Pattern {
    Uniform,
    BitPattern,
  };

  using ValueEntry = std::variant<APInt, Pattern>;

  std::string RegType;
  std::vector<ValueEntry> Values;
  std::vector<double> Weights;
};

struct RegisterHistograms {
  SmallVector<RegisterClassHistogram, 3> ClassHistograms;
};

struct RegistersWithHistograms {
  RegisterValues Registers;
  RegisterHistograms Histograms;
};

void checkRegisterClasses(const RegisterValues &Values,
                          ArrayRef<StringRef> AllowedClasses);

void checkRegisterClasses(const RegisterHistograms &Histograms,
                          ArrayRef<StringRef> AllowedClasses);

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<uint64_t> &Registers);

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<APInt> &Registers);

} // namespace llvm::snippy
