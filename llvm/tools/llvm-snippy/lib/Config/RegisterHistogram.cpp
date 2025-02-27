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
#include "snippy/Target/Target.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
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

void getFixedRegisterValues(const RegistersWithHistograms &RH,
                            size_t ExpectedNumber, StringRef Prefix,
                            unsigned NumBits, std::vector<APInt> &Result) {
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
    snippy::fatal("Registers file error",
                  "Unexpected number of registers found in for \"" + Prefix +
                      "\" group expecting " + Twine(ExpectedNumber) + ", got " +
                      Twine(NumFound));

  Result.resize(NumFound);
  std::transform(First, Last, Result.begin(), [&](const APInt &Value) {
    auto LeadingZeroBits = Value.countLeadingZeros();
    if (Value.getBitWidth() - LeadingZeroBits > NumBits) {
      SmallVector<char> Str;
      Value.toStringUnsigned(Str, 16);
      snippy::fatal("Registers file error",
                    "Entry " + Str + " for register type " + Prefix +
                        " is wider than requested bit width " + Twine(NumBits));
    }
    return Value.zextOrTrunc(NumBits);
  });
}

APInt sampleValuegramForOneReg(const Valuegram &Valuegram, StringRef Prefix,
                               unsigned NumBits,
                               std::discrete_distribution<size_t> &Dist) {
  auto &Engine = RandEngine::engine();
  const auto &Entry = Valuegram.at(Dist(Engine));

  using EntryKind = ValuegramEntry::EntryKind;
  auto Kind = Entry.getKind();
  switch (Kind) {
  case EntryKind::BitValue: {
    auto &ValueWithSign = cast<ValuegramBitValueEntry>(Entry.get()).ValWithSign;
    auto &Value = ValueWithSign.getVal();

    auto Width = Value.getActiveBits();
    if (Width > NumBits) {
      SmallVector<char> Str;
      Value.toString(Str, /*Radix=*/16,
                     /*Signed=*/ValueWithSign.Number.IsSigned,
                     /*formatAsCLiteral=*/true, /*UpperCase=*/false);
      LLVMContext Ctx;
      snippy::fatal(Ctx, "Failed to sample register histogram",
                    Twine("Histogram entry ")
                        .concat(Str)
                        .concat(" with ")
                        .concat(std::to_string(Value.getBitWidth()))
                        .concat(" bit width for register type ")
                        .concat(Prefix)
                        .concat(" is wider than requested bit width ")
                        .concat(Twine(NumBits)));
    }

    if (!ValueWithSign.isSigned())
      return Value.zextOrTrunc(NumBits);
    return Value.sextOrTrunc(NumBits);
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

  std::generate(Registers.begin(), Registers.end(), [&] {
    return sampleValuegramForOneReg(Valuegram, Prefix, NumBits, Dist);
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

void checkRegisterClasses(const AllRegisters &Values,
                          ArrayRef<StringRef> AllowedClasses,
                          const SnippyTarget *Tgt) {
  assert(!AllowedClasses.empty());
  for (const auto &ClassValues : Values.ClassValues) {
    auto It = std::find(AllowedClasses.begin(), AllowedClasses.end(),
                        ClassValues.RegType);
    if (It == AllowedClasses.end())
      snippy::fatal(formatv("Illegal register class {0} specified for initial "
                            "register values. The "
                            "following register "
                            "classes are legal for the current target: {1}.",
                            ClassValues.RegType,
                            formatAllowedRegisterClasses(AllowedClasses)));
  }
  if (Values.SpecialRegs.empty())
    return;
  auto RegNames = llvm::make_first_range(Values.SpecialRegs);
  auto UnknownRegisters = RegNames;
  if (UnknownRegisters.empty())
    return;
  std::string ErrorMessage;
  raw_string_ostream OS(ErrorMessage);
  OS << "List of unknown registers: [ ";
  llvm::interleaveComma(UnknownRegisters, OS);
  OS << " ]";
  snippy::fatal("Unknown registers detected in registogram", ErrorMessage);
}

void checkRegisterClasses(const RegisterHistograms &Histograms,
                          ArrayRef<StringRef> AllowedClasses) {
  assert(!AllowedClasses.empty());
  for (const auto &ClassHistogram : Histograms.ClassHistograms) {
    auto It = std::find(AllowedClasses.begin(), AllowedClasses.end(),
                        ClassHistogram.RegType);
    if (It == AllowedClasses.end())
      snippy::fatal(formatv("Illegal register class {0} specified in register "
                            "value histogram. The "
                            "following register "
                            "classes are legal for the current target: {1}.",
                            ClassHistogram.RegType,
                            formatAllowedRegisterClasses(AllowedClasses)));
  }
}

struct RegisterValuesEntry {
  RegisterValuesEntry() = default;
  RegisterValuesEntry(const std::string &RegName, APInt Val)
      : RegName(RegName), Value(Val) {}

  std::string RegName;
  APInt Value;
};

template <> struct YAMLHistogramTraits<RegisterValuesEntry> {
  using DenormEntry = RegisterValuesEntry;
  using MapType = AllRegisters;
  static constexpr bool ParseArbitraryValue = true;

  using RegTypeNumPair = std::pair<StringRef, unsigned>;

  static auto getSortedEntries(ArrayRef<RegisterValuesEntry> Entries) {
    std::vector<RegisterValuesEntry> EntriesCopy(Entries.begin(),
                                                 Entries.end());
    sort(EntriesCopy, [](const auto &L, const auto &R) {
      return StringRef(L.RegName).compare_numeric(StringRef(R.RegName)) < 0;
    });
    return EntriesCopy;
  }

  static auto findClassNameEnd(StringRef RegName) {
    auto Reversed = llvm::reverse(RegName);
    auto RIt =
        llvm::find_if_not(Reversed, [](auto C) { return std::isdigit(C); });
    assert(RIt != Reversed.end());
    return RIt.base();
  }

  static bool isRegWithClass(StringRef RegName) {
    auto ClassEnd = findClassNameEnd(RegName);
    return ClassEnd != RegName.end();
  }

  static auto getRegTypeAndIndex(StringRef RegName)
      -> Expected<std::pair<StringRef, unsigned>> {
    auto CreateError = [&]() {
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          "Could not derive register ordinal for " + RegName);
    };
    auto ClassEnd = findClassNameEnd(RegName);
    if (ClassEnd == RegName.end())
      return CreateError();
    auto ClassLength = std::distance(RegName.begin(), ClassEnd);
    if (ClassLength != 1)
      return CreateError();
    auto Class = RegName.substr(0, ClassLength);
    auto NumberStr = RegName.substr(ClassLength);
    unsigned Number = 0;
    bool Invalid = NumberStr.getAsInteger(10, Number);
    if (Invalid)
      return CreateError();
    return std::pair(Class, Number);
  }

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef RegName,
                                      StringRef RegValue) {
    APInt Value;
    auto ParseFailed = StringRef(RegValue).getAsInteger(0, Value);
    if (ParseFailed)
      Io.setError("Value " + Twine(RegValue) + " of register " +
                  Twine(RegName) + " is not an integer");
    return {RegName.str(), {Value}};
  }

  static void normalizeEntry(yaml::IO &Io, const RegisterValuesEntry &E,
                             SmallVectorImpl<SValue> &RawStrings) {
    const auto &Value = E.Value;
    assert(Value.getBitWidth() % 4 == 0);
    SmallString<64> Buffer;
    auto HexChars = Value.getBitWidth() / 4;
    Buffer.resize(HexChars, '0');
    Value.toStringUnsigned(Buffer, 16);
    RawStrings.push_back(E.RegName);
    RawStrings.push_back("0x" + Buffer.substr(Buffer.size() - HexChars).str());
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> Entries) {
    MapType Registers;
    std::map<std::string, std::map<unsigned, APInt>> RegClassToRegValues;
    for (auto &[RegName, Val] : Entries) {
      if (RegName == AllRegisters::PCRegName) {
        auto [ValuesIt, IsInsert] = RegClassToRegValues.try_emplace(RegName);
        assert(IsInsert);
        [[maybe_unused]] auto [ValIt, Inserted] =
            ValuesIt->second.try_emplace(0, Val);
        assert(Inserted);
      } else if (isRegWithClass(RegName)) {
        auto ExpectedClassIdx = getRegTypeAndIndex(RegName);
        if (auto Err = ExpectedClassIdx.takeError(); Err)
          Io.setError(toString(std::move(Err)));
        auto [RegClass, Idx] = *ExpectedClassIdx;
        auto [ValuesIt, _] = RegClassToRegValues.try_emplace(RegClass.str());
        [[maybe_unused]] auto [ValIt, Inserted] =
            ValuesIt->second.try_emplace(Idx, Val);
        assert(Inserted);
      } else {
        [[maybe_unused]] auto [ValIt, Inserted] =
            Registers.SpecialRegs.try_emplace(RegName, Val);
        assert(Inserted);
      }
    }
    for (auto &[Class, IdxValues] : RegClassToRegValues) {
      assert(!IdxValues.empty());
      auto Values = llvm::make_second_range(IdxValues);
      Registers.ClassValues.emplace_back(RegisterClassValues{
          Class, std::vector<APInt>(Values.begin(), Values.end())});
    }
    return Registers;
  }

  static void normalizeMap(yaml::IO &Io, const MapType &Registers,
                           std::vector<DenormEntry> &Entries) {
    for (const auto &[RegType, Values] : Registers.ClassValues)
      for (const auto &[Idx, Value] : enumerate(Values)) {
        auto RegName = RegType;
        if (RegType != AllRegisters::PCRegName)
          RegName += std::to_string(Idx);
        Entries.emplace_back(RegName, Value);
      }
    llvm::transform(Registers.SpecialRegs, std::back_inserter(Entries),
                    [](auto &RegVal) {
                      return RegisterValuesEntry{RegVal.first, RegVal.second};
                    });
  }

  static std::string
  validateDuplicate(ArrayRef<RegisterValuesEntry> SortedEntries,
                    ArrayRef<RegTypeNumPair> Regs) {
    std::set<std::string> Duplicates;
    auto RegNames =
        llvm::map_range(SortedEntries, [](auto &R) { return R.RegName; });
    auto DuplicateIt = std::adjacent_find(RegNames.begin(), RegNames.end());
    while (DuplicateIt != RegNames.end()) {
      Duplicates.emplace(*DuplicateIt);
      DuplicateIt = std::adjacent_find(std::next(DuplicateIt), RegNames.end());
    }
    if (Duplicates.empty())
      return "";
    std::string RegList;
    raw_string_ostream OS(RegList);
    llvm::interleaveComma(Duplicates, OS);
    return Twine("Duplicate register value detected for: ")
        .concat(RegList)
        .str();
  }

  static std::string
  validateNonConsecutive(ArrayRef<RegisterValuesEntry> SortedEntries,
                         ArrayRef<RegTypeNumPair> Regs) {
    auto NonConsecutiveIt =
        std::adjacent_find(Regs.begin(), Regs.end(), [](auto &&L, auto &&R) {
          return (std::get<StringRef>(L) == std::get<StringRef>(R)) &&
                 (std::get<unsigned>(L) + 1 != std::get<unsigned>(R));
        });

    if (NonConsecutiveIt == Regs.end())
      return "";

    auto RegName =
        SortedEntries[std::distance(Regs.begin(), std::next(NonConsecutiveIt))]
            .RegName;
    return Twine("Non-consecutive register group detected starting from ")
        .concat(RegName)
        .str();
  }

  static std::string
  validateStartsWith(ArrayRef<RegisterValuesEntry> SortedEntries,
                     ArrayRef<RegTypeNumPair> Regs) {
    auto DummyRegs = std::array{std::pair(StringRef(""), 0u)};
    auto RegsWithDummyGroup = concat<const RegTypeNumPair>(DummyRegs, Regs);
    auto IncorrectStartsWithIt = std::adjacent_find(
        RegsWithDummyGroup.begin(), RegsWithDummyGroup.end(),
        [](auto &&L, auto &&R) {
          return std::get<StringRef>(L) != std::get<StringRef>(R) &&
                 std::get<unsigned>(R) != 0;
        });

    if (IncorrectStartsWithIt == RegsWithDummyGroup.end())
      return "";

    auto &&RegType = std::get<StringRef>(*std::next(IncorrectStartsWithIt));
    return Twine("Register group \"")
        .concat(RegType)
        .concat("\" does not start with ")
        .concat(RegType)
        .concat("0")
        .str();
  }

  static std::string validate(ArrayRef<RegisterValuesEntry> Entries) {
    auto SortedEntries = getSortedEntries(Entries);
    std::vector<std::pair<StringRef, unsigned>> Regs;
    for (auto &&[RegName, _] : SortedEntries) {
      if (!isRegWithClass(RegName))
        continue;
      auto RegTypeAndNum = getRegTypeAndIndex(RegName);
      if (!RegTypeAndNum)
        return toString(RegTypeAndNum.takeError());
      Regs.push_back(*RegTypeAndNum);
    };

    auto InvokeValidate = [&SortedEntries, &Regs](auto PtrToFunc) {
      return PtrToFunc(SortedEntries, Regs);
    };

    auto ValidateFunctions = std::array{
        validateDuplicate, validateNonConsecutive, validateStartsWith};

    for (auto &&Func : ValidateFunctions) {
      if (auto ErrMsg = InvokeValidate(Func); !ErrMsg.empty())
        return ErrMsg;
    }

    return "";
  }
};

AllRegisters &AllRegisters::addRegisterGroup(StringRef Prefix,
                                             ArrayRef<uint64_t> Values) {
  std::vector<APInt> ConvertedValues;
  std::transform(Values.begin(), Values.end(),
                 std::back_inserter(ConvertedValues),
                 [](const auto &V) { return APInt(64, V); });
  return addRegisterGroup(Prefix, 64, ConvertedValues);
}

AllRegisters &AllRegisters::addRegisterGroup(StringRef RegType,
                                             unsigned BitsNum,
                                             ArrayRef<APInt> Values) {
  auto &CV = ClassValues.emplace_back();
  CV.RegType = RegType;
  for (const auto &[Idx, Value] : enumerate(Values)) {
    assert(Value.getBitWidth() == BitsNum);
    CV.Values.push_back(Value);
  }
  return *this;
}

void AllRegisters::saveAsYAML(raw_ostream &OS) {
  outputYAMLToStream(*this, OS);
}

RegistersWithHistograms loadRegistersFromYaml(StringRef Path) {
  return loadYAMLFromFileOrFatal<RegistersWithHistograms>(Path);
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

LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::RegisterValuesEntry)

template <> struct yaml::SequenceElementTraits<snippy::RegisterClassHistogram> {
  static constexpr bool flow = false;
};

using snippy::YAMLHistogramIO;

template <> struct yaml::MappingTraits<snippy::RegistersWithHistograms> {
  static void mapping(IO &Io, snippy::RegistersWithHistograms &Info) {
    YAMLHistogramIO<snippy::RegisterValuesEntry> RegValuesIO(Info.Registers);
    Io.mapOptional("registers", RegValuesIO);
    Io.mapOptional("histograms", Info.Histograms.ClassHistograms);
  }
};

template <> struct yaml::MappingTraits<snippy::AllRegisters> {
  static void mapping(IO &Io, snippy::AllRegisters &RegSer) {
    YAMLHistogramIO<snippy::RegisterValuesEntry> RegValuesIO(RegSer);
    Io.mapRequired("registers", RegValuesIO);
  }
};

} // namespace llvm
