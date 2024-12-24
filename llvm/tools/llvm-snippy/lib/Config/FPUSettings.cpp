//===-- FPUSettings.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/FPUSettings.h"
#include "snippy/Support/RandUtil.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/identity.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {

namespace {

Error checkOverflowIntegralConversionError(const APInt &Val,
                                           APFloat::opStatus Status,
                                           bool IsSigned) {
  if (Status != APFloat::opStatus::opOK &&
      Status != APFloat::opStatus::opInexact) {
    assert(Status & APFloat::opStatus::opOverflow);
    SmallString<16> StrVal;
    Val.toString(StrVal, /*Radix=*/16, /*Signed=*/IsSigned,
                 /*formatAsCLiteral=*/true, /*UpperCase=*/false);
    return createStringError(
        std::make_error_code(std::errc::result_out_of_range),
        Twine("Conversion from integer value '")
            .concat(StrVal)
            .concat("' would result in a floating-point overflow exception"));
  }

  return Error::success();
}

Error checkIntegralRangeOverflow(const snippy::FloatOverwriteRange &Range,
                                 RoundingMode RM,
                                 const fltSemantics &Semantics) {
  // (NOTE): Check that overflow does not occur. In case it does that means
  // that the range of values is too wide for the chosen floating-point type.
  // This should be possible only for half-precision types. Inexact operation
  // is allowed, since large integral values will not be exactly represented
  // as floats.
  //
  // From IEEE-754-2008 5.4.1 Arithmetic operations:
  // formatOf-convertFromInt(int)
  //
  // Integral values are converted exactly from integer formats to
  // floating-point formats whenever the value is representable in both
  // formats.
  //
  // If the converted value is not exactly representable in the destination
  // format, the result is determined according to the applicable
  // rounding-direction attribute, and an inexact or floating-point overflow
  // exception arises as specified in Clause 7.
  //
  // 7.4 Overflow
  //
  // In addition, under default exception handling for overflow, the
  // overflow flag shall be raised and the inexact
  // exception shall be signaled.

  auto CheckValue = [&](auto Val) -> Error {
    auto FPValue = APFloat{Semantics};
    constexpr auto IsSigned = std::is_signed_v<decltype(Val)>;
    auto SampledAPInt =
        APInt{CHAR_BIT * sizeof(decltype(Val)), static_cast<uint64_t>(Val),
              /*IsSigned=*/IsSigned};
    auto Status =
        FPValue.convertFromAPInt(SampledAPInt, /*IsSigned=*/IsSigned, RM);
    return checkOverflowIntegralConversionError(SampledAPInt, Status, IsSigned);
  };

  if (auto Err = CheckValue(Range.Min))
    return Err;

  if (auto Err = CheckValue(Range.Max))
    return Err;

  // If the overflow does not occur on the ends of the value range,
  // then for internal values it shall not occur.
  return Error::success();
}

Expected<const snippy::FloatOverwriteValues *>
getSettingsForSemantics(const snippy::FloatOverwriteSettings &Settings,
                        const fltSemantics &Semantics) {
  if (auto &Cfg = Settings.HalfValues; &Semantics == &APFloat::IEEEhalf())
    return Cfg ? &Cfg.value() : nullptr;
  if (auto &Cfg = Settings.SingleValues; &Semantics == &APFloat::IEEEsingle())
    return Cfg ? &Cfg.value() : nullptr;
  if (auto &Cfg = Settings.DoubleValues; &Semantics == &APFloat::IEEEdouble())
    return Cfg ? &Cfg.value() : nullptr;
  return createStringError(
      std::make_error_code(std::errc::invalid_argument),
      Twine("Requested floating-point semantics is not supported"));
}

Error createTooLargeForWidthError(uint32_t ValueWidth, Twine ValStr) {
  return createStringError(std::make_error_code(std::errc::result_out_of_range),
                           Twine("'")
                               .concat(ValStr)
                               .concat("' is too wide to fit in ")
                               .concat(Twine(ValueWidth))
                               .concat(" bits"));
}

} // namespace

using snippy::FPUSettings;
void yaml::MappingTraits<FPUSettings>::mapping(yaml::IO &IO, FPUSettings &Cfg) {
  IO.mapOptional("overwrite", Cfg.Overwrite);
}

using snippy::FloatOverwriteValues;
void yaml::MappingTraits<FloatOverwriteValues>::mapping(
    yaml::IO &IO, FloatOverwriteValues &Cfg) {
  IO.mapOptional("valuegram", Cfg.TheValuegram);
}

std::string
yaml::MappingTraits<FloatOverwriteValues>::validate(yaml::IO &IO,
                                                    FloatOverwriteValues &Cfg) {
  using snippy::RegisterClassHistogram;
  auto ContainsNegativeValues =
      any_of(Cfg.TheValuegram, [](const snippy::ValuegramEntry &Entry) {
        if (auto *BitValue =
                dyn_cast<snippy::ValuegramBitValueEntry>(&Entry.get()))
          return BitValue->isSigned();
        return false;
      });

  if (ContainsNegativeValues)
    return "Negative values are not allowed in a valuegram";

  return "";
}

using snippy::FloatOverwriteSettings;
void yaml::MappingTraits<FloatOverwriteSettings>::mapping(
    yaml::IO &IO, FloatOverwriteSettings &Cfg) {
  IO.mapOptional("mode", Cfg.Mode);
  IO.mapOptional("range", Cfg.IntegralRange);
  IO.mapOptional("ieee-half", Cfg.HalfValues);
  IO.mapOptional("ieee-single", Cfg.SingleValues);
  IO.mapOptional("ieee-double", Cfg.DoubleValues);
}

namespace snippy {
namespace {

template <typename T>
class IntRangeAsFloatSampler final : public IAPIntSampler {
public:
  template <typename C, typename D>
  IntRangeAsFloatSampler(C Min, D Max, RoundingMode Mode,
                         const fltSemantics &Semantics)
      : TheMin{Min}, TheMax{Max}, RM{Mode}, TheSemantics{Semantics} {
    assert(Min <= Max);
  }

  template <typename C, typename D>
  static Expected<IntRangeAsFloatSampler>
  create(C Min, D Max, RoundingMode Mode, const fltSemantics &Semantics) {
    if (Min > Max)
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          Twine("'max' should be greater or equal to 'min'"));

    return IntRangeAsFloatSampler(Min, Max, Mode, Semantics);
  }

  Expected<APInt> sample() override {
    auto FPValue = APFloat{TheSemantics};
    auto SampledIntValue = RandEngine::genInRangeInclusive(TheMin, TheMax);
    constexpr auto IsSigned = std::is_signed_v<T>;
    auto SampledAPInt =
        APInt{CHAR_BIT * sizeof(T), static_cast<uint64_t>(SampledIntValue),
              /*IsSigned=*/IsSigned};

    auto Status =
        FPValue.convertFromAPInt(SampledAPInt, /*IsSigned=*/IsSigned, RM);

    if (auto Err = checkOverflowIntegralConversionError(SampledAPInt, Status,
                                                        IsSigned))
      return Expected<APInt>(std::move(Err));

    return FPValue.bitcastToAPInt();
  }

private:
  T TheMin;
  T TheMax;
  RoundingMode RM;
  const fltSemantics &TheSemantics;
};

template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
Expected<APInt> createAPIntOfWidth(uint32_t BitWidth, T Val) {
  static constexpr auto IsSigned = std::is_signed_v<T>;
  static_assert(!IsSigned);
  auto LeadingZeros = countl_zero(Val);
  auto NumBits = sizeof(decltype(Val)) * CHAR_BIT;
  auto NumActiveBits = NumBits - LeadingZeros;
  if (NumActiveBits > BitWidth)
    return createTooLargeForWidthError(
        BitWidth, Twine("0x").concat(utohexstr(Val, /*LowerCase=*/true)));
  return APInt{BitWidth, static_cast<uint64_t>(Val),
               /*isSigned=*/IsSigned};
}

} // namespace

template <typename... Ts, typename = std::enable_if_t<std::conjunction_v<
                              std::is_same<std::decay_t<Ts>, APInt>...>>>
static std::optional<APInt> checkAPIntsInRange(uint32_t SemanticsSize,
                                               Ts &&...Vals) {
  auto CheckAPIntsImpl = [&](auto &&Self, const APInt &FrontVal,
                             const auto &...TailVals) -> std::optional<APInt> {
    auto CheckAPInt = [&](const APInt &Val) -> std::optional<APInt> {
      return Val.getActiveBits() > SemanticsSize ? std::optional{Val}
                                                 : std::nullopt;
    };

    auto FrontChecked = CheckAPInt(FrontVal);
    if constexpr (sizeof...(TailVals) >= 1)
      return FrontChecked ? FrontChecked : Self(Self, TailVals...);
    else
      return FrontChecked;
  };

  return CheckAPIntsImpl(CheckAPIntsImpl, Vals...);
}

static std::optional<APInt>
checkOutOfRangeValuegram(uint32_t SemanticsSize, const IValuegramEntry &Entry) {
  auto CheckAPInts = [SemanticsSize](auto &&...Vals) {
    return checkAPIntsInRange(SemanticsSize, Vals...);
  };

  return TypeSwitch<const IValuegramEntry *, std::optional<APInt>>(&Entry)
      .Case([&](const ValuegramBitValueEntry *BitValueEntry) {
        assert(BitValueEntry);
        return CheckAPInts(BitValueEntry->ValWithSign.Value);
      })
      .Case([&](const ValuegramBitRangeEntry *BitRangeEntry) {
        assert(BitRangeEntry);
        return CheckAPInts(BitRangeEntry->Min, BitRangeEntry->Max);
      })
      .Default([](auto &&) { return std::nullopt; });
}

static std::string
validateValuegramForSemantics(const Valuegram &VG,
                              const fltSemantics &Semantics) {
  auto SemanticsSize = APFloat::getSizeInBits(Semantics);

  // NOTE: During validation some entries might be null. That means that their
  // mapping was invalid and got diagnosed earlier on. We can safely skip them
  // and validate valid ones.
  auto NonNullEntries =
      make_filter_range(VG, [](const ValuegramEntry &Entry) -> bool {
        return Entry.getOrNull();
      });

  auto CheckedRange =
      map_range(NonNullEntries, [&](const ValuegramEntry &Entry) {
        return checkOutOfRangeValuegram(SemanticsSize, Entry.get());
      });

  if (auto EntryIt = find_if(
          CheckedRange,
          [](auto &&OptionalVal) -> bool { return OptionalVal.has_value(); });
      EntryIt != CheckedRange.end()) {
    auto OutOfRangeValue = *EntryIt;
    SmallString<sizeof(uint64_t) * CHAR_BIT / 4> ValStr;
    OutOfRangeValue->toString(ValStr, /*Radix=*/16, /*Signed=*/false,
                              /*formatAsCLiteral=*/true,
                              /*UpperCase=*/false);
    return toString(createTooLargeForWidthError(SemanticsSize, ValStr));
  }

  return "";
}

static Expected<std::unique_ptr<IAPIntSampler>>
createSamplerForEntry(const ValuegramEntry &Entry, uint32_t BitWidth) {
  return TypeSwitch<const IValuegramEntry *,
                    Expected<std::unique_ptr<IAPIntSampler>>>(&Entry.get())
      .Case([&](const ValuegramBitpatternEntry *) {
        return std::make_unique<BitPatternAPIntSamler>(BitWidth);
      })
      .Case([&](const ValuegramUniformEntry *) {
        return std::make_unique<UniformAPIntSamler>(BitWidth);
      })
      .Case([&](const ValuegramBitValueEntry *BitValueEntry)
                -> Expected<std::unique_ptr<IAPIntSampler>> {
        const auto &Val = BitValueEntry->ValWithSign.Value;
        if (Val.getActiveBits() > BitWidth) {
          SmallString<sizeof(uint64_t) * CHAR_BIT / 4> ValStr;
          Val.toString(ValStr, /*Radix=*/16, /*Signed=*/false,
                       /*formatAsCLiteral=*/true, /*UpperCase=*/false);
          return createTooLargeForWidthError(BitWidth, ValStr);
        }
        return std::make_unique<ConstantAPIntSampler>(
            Val.zextOrTrunc(BitWidth));
      })
      .Case([&](const ValuegramBitRangeEntry *BitRangeEntry)
                -> Expected<std::unique_ptr<IAPIntSampler>> {
        assert(BitRangeEntry);
        auto SamplerOrErr =
            APIntRangeSampler::create(BitRangeEntry->Min, BitRangeEntry->Max,
                                      /*IsSigned=*/false);
        if (Error E = SamplerOrErr.takeError())
          return Expected<std::unique_ptr<IAPIntSampler>>(std::move(E));
        return std::make_unique<APIntRangeSampler>(std::move(*SamplerOrErr));
      })
      .Default([](auto &&) {
        return createStringError(inconvertibleErrorCode(),
                                 Twine("Unhandled valuegram entry type"));
      });
}

} // namespace snippy

std::string yaml::MappingTraits<FloatOverwriteSettings>::validate(
    yaml::IO &IO, FloatOverwriteSettings &Cfg) {
  auto RM = RoundingMode::NearestTiesToEven;

  auto AvailableSemantics = std::array<const fltSemantics *, 3>{
      &APFloat::IEEEhalf(), &APFloat::IEEEsingle(), &APFloat::IEEEdouble()};

  auto GetSettingsForSemantics = [&](const fltSemantics *Semantics) {
    assert(Semantics);
    return cantFail(getSettingsForSemantics(Cfg, *Semantics));
  };

  if (Cfg.IntegralRange) {
    auto PresentSemantics = make_filter_range(
        AvailableSemantics, [&](const fltSemantics *Semantics) -> bool {
          return GetSettingsForSemantics(Semantics);
        });

    auto CheckedIntegralOverflowRange =
        map_range(PresentSemantics, [&](const fltSemantics *Semantics) {
          return checkIntegralRangeOverflow(*Cfg.IntegralRange, RM, *Semantics);
        });

    if (auto ErrIt = find_if_not(CheckedIntegralOverflowRange,
                                 [](Error Err) {
                                   auto Success = Err.success();
                                   snippy::burrowError(std::move(Err));
                                   return Success;
                                 });
        ErrIt != CheckedIntegralOverflowRange.end())
      return toString(*ErrIt);
  }

  {
    auto SettingForSemantics =
        map_range(AvailableSemantics, GetSettingsForSemantics);

    auto SemanticsWithSettings = zip(AvailableSemantics, SettingForSemantics);
    for (auto &&[Semantics, SettingsPtr] : SemanticsWithSettings) {
      if (!SettingsPtr)
        continue;
      if (auto ErrMsg = snippy::validateValuegramForSemantics(
              SettingsPtr->TheValuegram, *Semantics);
          !ErrMsg.empty())
        return ErrMsg;
    }
  }

  return "";
}

namespace {

struct NormalizedRoundingMode final {
  snippy::FPURoundingMode RMWrapper;
  NormalizedRoundingMode(yaml::IO &Io) {}
  NormalizedRoundingMode(yaml::IO &Io, const RoundingMode &Denorm)
      : RMWrapper{Denorm} {}
  RoundingMode denormalize(yaml::IO &Io) { return RMWrapper.value; }
};

} // namespace

void yaml::ScalarEnumerationTraits<snippy::FPURoundingMode>::enumeration(
    IO &Io, snippy::FPURoundingMode &Value) {
  auto EnumCase = [&](const char *Name, RoundingMode RM) {
    Io.enumCase(Value.value, Name, RM);
  };

  EnumCase("nearest-ties-to-even", RoundingMode::NearestTiesToEven);
  EnumCase("toward-zero", RoundingMode::TowardZero);
  EnumCase("toward-negative", RoundingMode::TowardNegative);
  EnumCase("toward-positive", RoundingMode::TowardPositive);
  EnumCase("nearest-ties-to-away", RoundingMode::NearestTiesToAway);

  // NOTE: When inputting data we allow shorthands like in risc-v isa manual
  // for conciseness.
  if (!Io.outputting()) {
    EnumCase("rne", RoundingMode::NearestTiesToEven);
    EnumCase("rtz", RoundingMode::TowardZero);
    EnumCase("rdn", RoundingMode::TowardNegative);
    EnumCase("rup", RoundingMode::TowardPositive);
    EnumCase("rmm", RoundingMode::NearestTiesToAway);
  }
}

using snippy::FloatOverwriteRange;
void yaml::MappingTraits<FloatOverwriteRange>::mapping(
    yaml::IO &Io, FloatOverwriteRange &Cfg) {
  yaml::MappingNormalization<NormalizedRoundingMode, RoundingMode>
      RoundingModeNorm(Io, Cfg.RM);
  Io.mapRequired("min", Cfg.Min);
  Io.mapRequired("max", Cfg.Max);
  Io.mapOptional("rounding-mode", RoundingModeNorm->RMWrapper);
  Io.mapOptional("weight", Cfg.Weight, 1.0);
}

std::string
yaml::MappingTraits<FloatOverwriteRange>::validate(yaml::IO &IO,
                                                   FloatOverwriteRange &Cfg) {
  if (Cfg.Min > Cfg.Max)
    return "'max' should be greater or equal to 'min'";
  return "";
}

using snippy::FloatOverwriteMode;
using snippy::FloatOverwriteModeName;
#ifdef ENUM_CASE
#error ENUM_CASE already defined
#endif
#define ENUM_CASE(name)                                                        \
  IO.enumCase(M, FloatOverwriteModeName<FloatOverwriteMode::name>.data(),      \
              FloatOverwriteMode::name)

void yaml::ScalarEnumerationTraits<FloatOverwriteMode>::enumeration(
    yaml::IO &IO, FloatOverwriteMode &M) {
  ENUM_CASE(IF_ALL_OPERANDS);
  ENUM_CASE(IF_ANY_OPERAND);
  ENUM_CASE(DISABLED);
  ENUM_CASE(IF_MODEL_DETECTED_NAN);
}
#undef ENUM_CASE

namespace snippy {

Expected<std::unique_ptr<IAPIntSampler>>
createFloatOverwriteValueSampler(const FloatOverwriteSettings &Settings,
                                 const fltSemantics &Semantics) {
  snippy::WeightedAPIntSamplerSetBuilder Builder;
  auto BitWidth = APFloat::semanticsSizeInBits(Semantics);

  Expected<const FloatOverwriteValues *> CfgOrErr =
      getSettingsForSemantics(Settings, Semantics);

  if (auto Err = CfgOrErr.takeError())
    return Expected<std::unique_ptr<IAPIntSampler>>(std::move(Err));

  // (NOTE): Fallback case. The most reasonable choice for default
  // distribution is uniform.
  auto BuildWithFallback = [&]() {
    if (Builder.isEmpty())
      Builder.addOwned(std::make_unique<UniformAPIntSamler>(BitWidth), 1.0);
    return Builder.build();
  };

  if (const auto &IntegralRange = Settings.IntegralRange) {
    auto SamplerOrErr =
        IntRangeAsFloatSampler<FloatOverwriteRange::ValueType>::create(
            IntegralRange->Min, IntegralRange->Max, IntegralRange->RM,
            Semantics);

    if (auto E = SamplerOrErr.takeError())
      return Expected<std::unique_ptr<IAPIntSampler>>(std::move(E));

    Builder.addOwned(std::move(*SamplerOrErr), IntegralRange->Weight);
  }

  auto *Cfg = *CfgOrErr;
  if (!Cfg)
    return BuildWithFallback();

  for (const auto &Entry : Cfg->TheValuegram) {
    auto SamplerOrErr = createSamplerForEntry(Entry, BitWidth);
    if (Error E = SamplerOrErr.takeError())
      return Expected<std::unique_ptr<IAPIntSampler>>(std::move(E));
    Builder.addOwned(std::move(*SamplerOrErr), Entry.getWeight());
  }

  return BuildWithFallback();
}

Expected<IAPIntSampler &>
FloatSemanticsSamplerHolder::getSamplerFor(const fltSemantics &Semantics) {
  auto FoundIt = FloatValueSamplerForSemantics.find(&Semantics);

  if (FoundIt != FloatValueSamplerForSemantics.end())
    return *FoundIt->second;

  auto SamplerOrErr =
      createFloatOverwriteValueSampler(OverwriteSettings, Semantics);
  auto [InsertedIt, _] =
      FloatValueSamplerForSemantics.emplace(&Semantics, nullptr);
  if (auto Err = std::move(SamplerOrErr).moveInto(InsertedIt->second))
    return Expected<IAPIntSampler &>(std::move(Err));
  return *InsertedIt->second;
}

} // namespace snippy
} // namespace llvm
