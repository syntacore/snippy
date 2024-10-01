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

#include <string>
#include <variant>
#include <vector>

namespace llvm {
namespace snippy {

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

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::RegisterClassHistogram);

} // namespace llvm
