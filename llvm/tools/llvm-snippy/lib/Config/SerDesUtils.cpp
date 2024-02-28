//===-- SerDesUtils.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/SerDesUtils.h"
#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Support/Utils.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/YAMLTraits.h"

#include <algorithm>
#include <optional>

namespace llvm {
namespace snippy {

static std::optional<RegisterClassHistogram::Pattern>
parseHistogramPattern(StringRef ParseStr) {
  if (ParseStr == "uniform")
    return RegisterClassHistogram::Pattern::Uniform;
  if (ParseStr == "bitpattern")
    return RegisterClassHistogram::Pattern::BitPattern;
  return std::nullopt;
}

static StringRef histogramPatternToString(RegisterClassHistogram::Pattern P) {
  switch (P) {
  case RegisterClassHistogram::Pattern::Uniform:
    return "uniform";
  case RegisterClassHistogram::Pattern::BitPattern:
    return "bitpattern";
  }
  llvm_unreachable("Broken enum");
}

struct RegisterClassHistogramEntry {
  RegisterClassHistogram::ValueEntry Value;
  double Weight;
};

template <> struct YAMLHistogramTraits<RegisterClassHistogramEntry> {
  using DenormEntry = RegisterClassHistogramEntry;
  using MapType = RegisterClassHistogram;

  static unsigned getAutoSenseRadix(StringRef &Str) {
    if (Str.empty())
      return 10;
    if (Str.startswith("0x") || Str.startswith("0X")) {
      return 16;
    }
    if (Str.startswith("0b") || Str.startswith("0B")) {
      return 2;
    }
    if (Str.startswith("0o")) {
      return 8;
    }
    if (Str[0] == '0' && Str.size() > 1 && isDigit(Str[1])) {
      return 8;
    }
    return 10;
  }

  static auto reportError(yaml::IO &Io, const Twine &Msg) {
    Io.setError(Msg);
    return RegisterClassHistogram::ValueEntry{};
  }

  static RegisterClassHistogram::ValueEntry parseValue(yaml::IO &Io,
                                                       StringRef ParseStr) {
    StringRef OriginalStr = ParseStr;
    auto Pattern = parseHistogramPattern(ParseStr);
    if (Pattern)
      return *Pattern;
    APInt Value;
    if (ParseStr.empty())
      return reportError(Io, "Histogram entry value empty");
    bool IsNegative = false;
    // We remove minus because StringRef::getAsInteger method overload for APInt
    // doesn't handle the minus.
    if (ParseStr.front() == '-') {
      IsNegative = true;
      ParseStr = ParseStr.substr(1);
    }
    if (IsNegative && getAutoSenseRadix(ParseStr) != 10)
      return reportError(Io, "Histogram entry value " + OriginalStr +
                                 " is negative, but "
                                 "the radix is not equal to 10");
    auto ParseFailed = ParseStr.getAsInteger(0, Value);
    if (ParseFailed)
      return reportError(Io, "Histogram entry value " + OriginalStr +
                                 " is not an integer or a valid pattern");
    // If there was a minus, we negate the number, if not - add one zero at the
    // beginning to interpreted as positive.
    if (!IsNegative)
      return Value.zext(Value.getBitWidth() + 1);
    Value.negate();
    return Value;
  }

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef ValueStr,
                                      double Weight) {
    return {parseValue(Io, ValueStr), Weight};
  }

  static void normalizeEntry(yaml::IO &Io, const DenormEntry &E,
                             SmallVectorImpl<SValue> &RawStrings) {
    struct NameVisitor {
      std::string operator()(RegisterClassHistogram::Pattern P) {
        return histogramPatternToString(P).str();
      }
      std::string operator()(APInt Val) {
        SmallString<16> Str;
        Val.toStringUnsigned(Str);
        return Str.str().str();
      }
    };
    RawStrings.push_back(std::visit(NameVisitor{}, E.Value));
    RawStrings.push_back(std::to_string(E.Weight));
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> Entries) {
    RegisterClassHistogram Hist;
    transform(Entries, std::back_inserter(Hist.Values),
              [](auto &&E) { return E.Value; });
    transform(Entries, std::back_inserter(Hist.Weights),
              [](auto &&E) { return E.Weight; });
    return Hist;
  }

  static void normalizeMap(yaml::IO &Io, const RegisterClassHistogram &Hist,
                           std::vector<DenormEntry> &Entries) {
    transform(zip(Hist.Values, Hist.Weights), std::back_inserter(Entries),
              [](auto &&Pair) {
                auto &&[Value, Weight] = Pair;
                return DenormEntry{Value, Weight};
              });
  }

  static std::string validate(ArrayRef<DenormEntry> Entries) { return ""; }
};

struct RegisterValuesEntry {
  std::string RegName;
  APInt Value;
};

template <> struct YAMLHistogramTraits<RegisterValuesEntry> {
  using DenormEntry = RegisterValuesEntry;
  using MapType = RegisterValues;
  static constexpr bool ParseArbitraryValue = true;

  static auto getRegTypeAndNumber(StringRef RegName)
      -> Expected<std::pair<StringRef, unsigned>> {
    auto RegStr = RegName.take_front();
    auto NumberStr = RegName.drop_front();
    unsigned Number = 0;
    bool Invalid = NumberStr.getAsInteger(10, Number);
    if (Invalid)
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          "Could not derive register ordinal for " + RegName);
    return std::pair(RegStr, Number);
  };

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef RegName,
                                      StringRef RegValue) {
    APInt Value;
    auto ParseFailed = StringRef(RegValue).getAsInteger(0, Value);
    if (ParseFailed)
      Io.setError("Value " + Twine(RegValue) + " of register " +
                  Twine(RegName) + " is not an integer");
    if (auto Err = getRegTypeAndNumber(RegName).takeError(); Err)
      Io.setError(toString(std::move(Err)));
    return DenormEntry{RegName.str(), {Value}};
  }

  static void normalizeEntry(yaml::IO &Io, const DenormEntry &E,
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

  static auto getSortedEntries(ArrayRef<DenormEntry> Entries) {
    std::vector<DenormEntry> EntriesCopy(Entries);
    sort(EntriesCopy, [](const auto &L, const auto &R) {
      return StringRef(L.RegName).compare_numeric(StringRef(R.RegName)) < 0;
    });
    return EntriesCopy;
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> Entries) {
    MapType Registers;
    auto &ClassValues = Registers.ClassValues;
    auto EntriesSorted = getSortedEntries(Entries);

    auto GetRegType = [](auto &&RegName) {
      auto RegTypeAndNum = getRegTypeAndNumber(RegName);
      [[maybe_unused]] auto Invalid = errorToBool(RegTypeAndNum.takeError());
      assert(!Invalid);
      auto [RegType, _] = *RegTypeAndNum;
      return RegType;
    };

    auto GetNextChunkByIt = [&EntriesSorted, GetRegType](auto It) {
      return std::adjacent_find(
          It, EntriesSorted.end(), [GetRegType](auto &&L, auto &&R) {
            return GetRegType(L.RegName) != GetRegType(R.RegName);
          });
    };

    for (auto ChunkIt = EntriesSorted.begin();
         ChunkIt != EntriesSorted.end();) {
      auto ChunkEnd = GetNextChunkByIt(ChunkIt);
      if (ChunkEnd != EntriesSorted.end())
        ++ChunkEnd;

      ClassValues.push_back({GetRegType(ChunkIt->RegName).str(), {}});

      for (auto &&[_, Value] : make_range(ChunkIt, ChunkEnd)) {
        auto &LastTypeValues = ClassValues.back().Values;
        LastTypeValues.push_back(Value);
      }

      ChunkIt = ChunkEnd;
    }

    return Registers;
  }

  static void normalizeMap(yaml::IO &Io, const MapType &Registers,
                           std::vector<DenormEntry> &Entries) {
    for (const auto &[RegType, Values] : Registers.ClassValues)
      for (const auto &[Idx, Value] : enumerate(Values))
        Entries.push_back({RegType + std::to_string(Idx), Value});
  }

  using RegTypeNumPair = std::pair<StringRef, unsigned>;
  static std::string validateDuplicate(ArrayRef<DenormEntry> SortedEntries,
                                       ArrayRef<RegTypeNumPair> Regs) {
    auto DuplicateIt = std::adjacent_find(Regs.begin(), Regs.end());
    if (DuplicateIt == Regs.end())
      return "";
    auto RegName =
        SortedEntries[std::distance(Regs.begin(), std::next(DuplicateIt))]
            .RegName;
    return ("Duplicate register value detected for " + Twine(RegName)).str();
  }

  static std::string validateNonConsecutive(ArrayRef<DenormEntry> SortedEntries,
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
    return ("Non-consecutive register group detected starting from " +
            Twine(RegName))
        .str();
  }

  static std::string validateStartsWith(ArrayRef<DenormEntry> SortedEntries,
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
    return ("Register group \"" + RegType + "\" does not start with " +
            RegType + "0")
        .str();
  }

  static std::string validate(ArrayRef<DenormEntry> Entries) {
    auto SortedEntries = getSortedEntries(Entries);
    std::vector<std::pair<StringRef, unsigned>> Regs;
    for (auto &&[RegName, _] : SortedEntries) {
      auto RegTypeAndNum = getRegTypeAndNumber(RegName);
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

template <> struct YAMLHistogramTraits<ImmediateHistogramEntry> {
  using DenormEntry = ImmediateHistogramEntry;
  using MapType = ImmediateHistogramSequence;

  static DenormEntry denormalizeEntry(yaml::IO &Io, StringRef ParseStr,
                                      double Weight) {
    int Value = 0;
    auto ParseFailed = ParseStr.getAsInteger(0, Value);
    if (ParseFailed) {
      Io.setError("Immediate histogram entry value " + ParseStr +
                  " is not an integer");
    }
    return {Value, Weight};
  }

  static void normalizeEntry(yaml::IO &Io, const DenormEntry &E,
                             SmallVectorImpl<SValue> &RawStrings) {
    RawStrings.push_back(std::to_string(E.Value));
    RawStrings.push_back(std::to_string(E.Weight));
  }

  static MapType denormalizeMap(yaml::IO &Io, ArrayRef<DenormEntry> Entries) {
    MapType Hist;
    SmallVector<DenormEntry> CopiedEntries(Entries);
    sort(CopiedEntries, [](DenormEntry LHS, DenormEntry RHS) {
      return LHS.Value < RHS.Value;
    });
    transform(CopiedEntries, std::back_inserter(Hist.Values),
              [](DenormEntry E) { return E.Value; });
    transform(CopiedEntries, std::back_inserter(Hist.Weights),
              [](DenormEntry E) { return E.Weight; });
    return Hist;
  }

  static void normalizeMap(yaml::IO &Io, const MapType &Hist,
                           std::vector<DenormEntry> &Entries) {
    transform(zip(Hist.Values, Hist.Weights), std::back_inserter(Entries),
              [](auto &&P) {
                auto &&[Value, Weight] = P;
                return DenormEntry{Value, Weight};
              });
  }

  static std::string validate(ArrayRef<DenormEntry> Entries) { return ""; }
};

RegisterSerialization &
RegisterSerialization::addRegisterGroup(StringRef Prefix,
                                        ArrayRef<uint64_t> Values) {
  std::vector<APInt> ConvertedValues;
  std::transform(Values.begin(), Values.end(),
                 std::back_inserter(ConvertedValues),
                 [](const auto &V) { return APInt(64, V); });
  return addRegisterGroup(Prefix, 64, ConvertedValues);
}

RegisterSerialization &
RegisterSerialization::addRegisterGroup(StringRef RegType, unsigned BitsNum,
                                        ArrayRef<APInt> Values) {
  auto &ClassValues = Registers.ClassValues.emplace_back();
  ClassValues.RegType = RegType;
  for (const auto &[Idx, Value] : enumerate(Values)) {
    assert(Value.getBitWidth() == BitsNum);
    ClassValues.Values.push_back(Value);
  }
  return *this;
}

void RegisterSerialization::saveAsYAML(raw_ostream &OS) {
  outputYAMLToStream(*this, OS);
}

RegistersWithHistograms loadRegistersFromYaml(StringRef Path) {
  return loadYAMLFromFileOrFatal<RegistersWithHistograms>(Path);
}

} // namespace snippy

LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::RegisterClassHistogramEntry)
LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::RegisterValuesEntry)
LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(snippy::ImmediateHistogramEntry)

template <> struct yaml::SequenceElementTraits<snippy::RegisterClassHistogram> {
  static constexpr bool flow = false;
};

using snippy::YAMLHistogramIO;

template <> struct yaml::MappingTraits<snippy::RegisterClassHistogram> {
  static void mapping(IO &Io, snippy::RegisterClassHistogram &Hist) {
    YAMLHistogramIO<snippy::RegisterClassHistogramEntry> RegHistogramIO(Hist);

    // Very important! Do not change the order of these two lines. Otherwise you
    // will summon the kraken.
    Io.mapRequired("values", RegHistogramIO);
    Io.mapRequired("reg-type", RegHistogramIO.UnderlyingMap.RegType);
  }
};

template <> struct yaml::MappingTraits<snippy::RegistersWithHistograms> {
  static void mapping(IO &Io, snippy::RegistersWithHistograms &Info) {
    YAMLHistogramIO<snippy::RegisterValuesEntry> RegValuesIO(Info.Registers);
    Io.mapOptional("registers", RegValuesIO);
    Io.mapOptional("histograms", Info.Histograms.ClassHistograms);
  }
};

template <> struct yaml::MappingTraits<snippy::RegisterSerialization> {
  static void mapping(IO &Io, snippy::RegisterSerialization &RegSer) {
    YAMLHistogramIO<snippy::RegisterValuesEntry> RegValuesIO(RegSer.Registers);
    Io.mapRequired("registers", RegValuesIO);
  }
};

void yaml::MappingTraits<snippy::ImmediateHistogramSequence>::mapping(
    IO &Io, snippy::ImmediateHistogramSequence &ImmHist) {
  YAMLHistogramIO<snippy::ImmediateHistogramEntry> ImmHistIO(ImmHist);
  EmptyContext Ctx;
  yamlize(Io, ImmHistIO, false, Ctx);
}

} // namespace llvm
