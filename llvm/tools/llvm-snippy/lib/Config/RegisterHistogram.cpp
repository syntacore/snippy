//===-- RegisterHistogram.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Support/APIntSampler.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLHistogram.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/YAMLTraits.h"

#include <algorithm>

namespace llvm {
namespace snippy {

static Expected<APInt>
getValueFromHistogramPattern(ValuegramEntry::EntryKind Pattern,
                             unsigned NumBits) {
  switch (Pattern) {
  case ValuegramEntry::EntryKind::Uniform:
    return UniformAPIntSamler::generate(NumBits);
  case ValuegramEntry::EntryKind::BitPattern:
    return BitPatternAPIntSamler::generate(NumBits);
  default:
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Not a histogram pattern");
  }
}

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
    // FIXME: Get rid of report_fatal_error and propagate errors cleanly.
    report_fatal_error("Unexpected number of registers found in for \"" +
                           Prefix + "\" group expecting " +
                           Twine(ExpectedNumber) + ", got " + Twine(NumFound),
                       false);

  Result.resize(NumFound);
  std::transform(First, Last, Result.begin(),
                 [&](const APInt &Value) { return Value.zext(NumBits); });
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
  const auto &Valuegram = Hist.TheValuegram;

  std::discrete_distribution<size_t> Dist(Valuegram.weights_begin(),
                                          Valuegram.weights_end());

  auto &Engine = RandEngine::engine();
  std::generate(Registers.begin(), Registers.end(), [&] {
    const auto &Entry = Valuegram.at(Dist(Engine));

    using EntryKind = ValuegramEntry::EntryKind;
    auto Kind = Entry.getKind();
    switch (Kind) {
    case EntryKind::BitValue: {
      auto &ValueWithSign =
          cast<ValuegramBitValueEntry>(Entry.get()).ValWithSign;
      auto &Value = ValueWithSign.Value;

      if (Value.getActiveBits() > NumBits) {
        SmallVector<char> Str;
        Value.toString(Str, /*Radix=*/16, /*Signed=*/ValueWithSign.IsSigned,
                       /*formatAsCLiteral=*/true, /*UpperCase=*/false);
        LLVMContext Ctx;
        snippy::fatal(Ctx, "Failed to sample register histogram",
                      Twine("Histogram entry ")
                          .concat(Str)
                          .concat(" for register type ")
                          .concat(Prefix)
                          .concat(" is wider than requested bit width ")
                          .concat(Twine(NumBits)));
      }

      if (ValueWithSign.IsSigned)
        return Value.sextOrTrunc(NumBits);
      return Value.zextOrTrunc(NumBits);
    }
    case EntryKind::BitPattern:
    case EntryKind::Uniform: {
      APInt Val;
      if (Error E = getValueFromHistogramPattern(Kind, NumBits).moveInto(Val)) {
        LLVMContext Ctx;
        snippy::fatal(Ctx, "Failed to sample register histogram", std::move(E));
      }
      return Val;
    }
    default:
      llvm_unreachable("Unhandled register value histogram entry variant");
    }
  });
}

void getRegisterGroup(const RegistersWithHistograms &RH, size_t ExpectedNumber,
                      StringRef Prefix, unsigned NumBits,
                      std::vector<uint64_t> &Registers) {
  std::vector<APInt> APInts(ExpectedNumber);
  sampleHistogramForRegType(RH, Prefix, NumBits, APInts);
  if (APInts.empty()) {
    getFixedRegisterValues(RH, ExpectedNumber, Prefix, 64, APInts);
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
      report_fatal_error("Illegal register class " + ClassValues.RegType +
                             " specified for initial register values. The "
                             "following register "
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

} // namespace snippy

void yaml::MappingTraits<snippy::RegisterClassHistogram>::mapping(
    IO &Io, snippy::RegisterClassHistogram &Hist) {
  Io.mapRequired("values", Hist.TheValuegram);
  Io.mapRequired("reg-type", Hist.RegType);
}

std::string yaml::MappingTraits<snippy::RegisterClassHistogram>::validate(
    IO &Io, snippy::RegisterClassHistogram &Hist) {
  auto MapEntries = map_range(Hist.TheValuegram, [](auto &&Entry) {
    return dyn_cast_or_null<snippy::IValuegramMapEntry>(Entry.getOrNull());
  });

  for (auto *MapEntry : MapEntries) {
    if (!MapEntry)
      continue;

    if (auto Msg = MapEntry->validate(Io); Msg.size())
      return Msg;
  }

  return "";
}

} // namespace llvm
