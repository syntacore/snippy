//===-- RegisterHistogram.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Support/RandUtil.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::snippy;

static void getFixedRegisterValues(const RegistersWithHistograms &RH,
                                   size_t ExpectedNumber, StringRef Prefix,
                                   unsigned NumBits,
                                   std::vector<APInt> &Result) {
  assert((NumBits % CHAR_BIT) == 0);

  const auto &ClassValues = RH.Registers.ClassValues;
  auto It = std::find_if(
      ClassValues.begin(), ClassValues.end(),
      [&](const RegisterClassValues &CV) { return CV.RegType == Prefix; });
  if (It == ClassValues.end()) {
    Result.clear();
    return;
  }

  auto First = It->Values.begin();
  auto Last = It->Values.end();

  unsigned NumFound = std::distance(First, Last);
  assert(NumFound > 0);

  // NOTE: for now we require that either all registers are present or none of
  // them are. This may be changed in future
  if (ExpectedNumber != NumFound)
    report_fatal_error("Unexpected number of registers found in for \"" +
                           Prefix + "\" group expecting " +
                           Twine(ExpectedNumber) + ", got " + Twine(NumFound),
                       false);

  Result.resize(NumFound);
  std::transform(First, Last, Result.begin(),
                 [&](const APInt &Value) { return Value.zext(NumBits); });
}

static APInt generateBitPattern(unsigned NumBits) {
  auto Result = APInt::getZero(NumBits);
  auto Stride = RandEngine::genInRange<unsigned>(1, NumBits);
  auto Idx = RandEngine::genInInterval<unsigned>(0, Stride);
  while (Idx < NumBits) {
    Result.insertBits(1, Idx, 1);
    Idx += Stride;
  }
  if (RandEngine::genInRange(0, 1))
    return ~Result;
  return Result;
}

static APInt
getValueFromHistogramPattern(RegisterClassHistogram::Pattern Pattern,
                             unsigned NumBits) {
  switch (Pattern) {
  case RegisterClassHistogram::Pattern::Uniform: {
    return RandEngine::genAPInt(NumBits);
  }
  case RegisterClassHistogram::Pattern::BitPattern: {
    return generateBitPattern(NumBits);
  }
  }
  llvm_unreachable("Unhandled histogram pattern");
}

static void sampleHistogramForRegType(const RegistersWithHistograms &RH,
                                      StringRef Prefix, unsigned NumBits,
                                      std::vector<APInt> &Registers) {
  const auto &ClassHistograms = RH.Histograms.ClassHistograms;
  auto It = std::find_if(
      ClassHistograms.begin(), ClassHistograms.end(),
      [&](const RegisterClassHistogram &CH) { return CH.RegType == Prefix; });
  if (It == ClassHistograms.end()) {
    Registers.clear();
    return;
  }
  const auto &Hist = *It;
  std::discrete_distribution<size_t> Dist(Hist.Weights.begin(),
                                          Hist.Weights.end());
  auto &Engine = RandEngine::engine();
  std::generate(Registers.begin(), Registers.end(), [&] {
    const auto &Entry = Hist.Values[Dist(Engine)];
    if (const auto *Value = std::get_if<APInt>(&Entry)) {
      auto LeadingZeroBits = Value->countLeadingZeros();
      if (Value->getBitWidth() - LeadingZeroBits > NumBits) {
        SmallVector<char> Str;
        Value->toStringUnsigned(Str, 16);
        report_fatal_error("Histogram entry " + Str + " for register type " +
                               Prefix + " is wider than requested bit width " +
                               Twine(NumBits),
                           false);
      }
      return Value->sextOrTrunc(NumBits);
    }
    if (const auto *Value =
            std::get_if<RegisterClassHistogram::Pattern>(&Entry))
      return getValueFromHistogramPattern(*Value, NumBits);
    llvm_unreachable("Unhandled register value histogram entry variant");
  });
}

namespace llvm::snippy {

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<uint64_t> &Registers) {
  std::vector<APInt> APInts(ExpectedNumber);
  sampleHistogramForRegType(RH, Prefix, NumBits, APInts);
  if (APInts.empty()) {
    getFixedRegisterValues(RH, ExpectedNumber, Prefix, 64, APInts);
    // TODO: decide whether values that don't fit in registers should be
    // truncated or raise an error
#if 0
    for (auto Value : Registers)
      if (Value > MaxValue) {
        report_fatal_error(
            "Register value " + Twine(Value) + " for register of type " +
                Prefix +
                " is outside the valid range of values for this register type",
            false);
      }
#endif
  }
  Registers.resize(APInts.size());
  std::transform(APInts.begin(), APInts.end(), Registers.begin(),
                 [](const APInt &Value) { return Value.getZExtValue(); });
}

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned BitsNum,
                      std::vector<APInt> &Registers) {
  Registers.resize(ExpectedNumber);
  sampleHistogramForRegType(RH, Prefix, BitsNum, Registers);
  if (Registers.empty())
    getFixedRegisterValues(RH, ExpectedNumber, Prefix, BitsNum, Registers);
}

static auto formatAllowedRegisterClasses(ArrayRef<StringRef> AllowedClasses) {
  assert(!AllowedClasses.empty());
  SmallString<64> AllowedClassesStr = AllowedClasses.front();
  for (auto C : AllowedClasses.slice(1)) {
    AllowedClassesStr.append(", ");
    AllowedClassesStr.append(C);
  }
  return AllowedClassesStr;
}

void checkRegisterClasses(const RegisterValues &Values,
                          ArrayRef<StringRef> AllowedClasses) {
  assert(!AllowedClasses.empty());
  for (const auto &ClassValues : Values.ClassValues) {
    auto It = std::find(AllowedClasses.begin(), AllowedClasses.end(),
                        ClassValues.RegType);
    if (It == AllowedClasses.end())
      report_fatal_error(
          "Illegal register class " + ClassValues.RegType +
              " specified for initial register values. The following register "
              "classes are legal for the current target: " +
              formatAllowedRegisterClasses(AllowedClasses) + ".",
          false);
  }
}

void checkRegisterClasses(const RegisterHistograms &Histograms,
                          ArrayRef<StringRef> AllowedClasses) {
  assert(!AllowedClasses.empty());
  for (const auto &ClassHistogram : Histograms.ClassHistograms) {
    auto It = std::find(AllowedClasses.begin(), AllowedClasses.end(),
                        ClassHistogram.RegType);
    if (It == AllowedClasses.end())
      report_fatal_error("Illegal register class " + ClassHistogram.RegType +
                             " specified in "
                             "register value histogram. The following register "
                             "classes are legal for the current target: " +
                             formatAllowedRegisterClasses(AllowedClasses) + ".",
                         false);
  }
}

} // namespace llvm::snippy
