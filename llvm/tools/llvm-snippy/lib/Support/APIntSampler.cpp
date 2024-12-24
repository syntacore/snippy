//===-- APIntSampler.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/APIntSampler.h"
#include "snippy/Support/RandUtil.h"

#include "llvm/ADT/STLExtras.h"

namespace llvm::snippy {

Expected<APInt> APIntRangeSampler::sample() {
  // (FIXME): Rip out global engine.

  if (!TheIsSigned) {
    auto Dist = UniformIntDistribution<LargestUnsignedNativeType>(
        TheMin.getZExtValue(), TheMax.getZExtValue());
    auto Val = Dist(RandEngine::engine());
    return llvm::APInt{getBitWidth(), Val, /*isSigned=*/false};
  }

  auto Dist = UniformIntDistribution<LargestSignedNativeType>(
      TheMin.getSExtValue(), TheMax.getSExtValue());
  auto Val = Dist(RandEngine::engine());
  // (NOTE): APInt constructor accepts a uint64_t and reinterperts it as a
  // signed value.
  return llvm::APInt{getBitWidth(), static_cast<LargestUnsignedNativeType>(Val),
                     /*isSigned=*/true};
}

APInt BitPatternAPIntSamler::generate(uint32_t NumBits) {
  auto Result = APInt::getZero(NumBits);
  auto Stride = RandEngine::genInRangeExclusive<unsigned>(1, NumBits);
  auto Idx = RandEngine::genInRangeInclusive<unsigned>(0, Stride);
  while (Idx < NumBits) {
    Result.insertBits(1, Idx, 1);
    Idx += Stride;
  }
  if (RandEngine::genBool())
    return ~Result;
  return Result;
}

APInt UniformAPIntSamler::generate(uint32_t NumBits) {
  return RandEngine::genAPInt(NumBits);
}

namespace {

struct WeightedAPIntSamplerSet final : public IAPIntSampler {
  std::discrete_distribution<std::size_t> Dist;
  std::vector<std::unique_ptr<IAPIntSampler>> OwnedSamplers;

  template <typename T>
  WeightedAPIntSamplerSet(T &&WeightedSamplers)
      : Dist([&]() {
          auto WeightRange = llvm::map_range(
              WeightedSamplers, [](auto &&Val) { return Val.Weight; });
          return decltype(Dist)(WeightRange.begin(), WeightRange.end());
        }()) {
    copy(map_range(WeightedSamplers,
                   [](auto &&Val) { return std::move(Val.Sampler); }),
         std::back_inserter(OwnedSamplers));
  }

  Expected<APInt> sample() override {
    auto Idx = Dist(RandEngine::engine());
    auto &ChosenSampler = *OwnedSamplers[Idx];
    return ChosenSampler.sample();
  }
};

} // namespace

std::unique_ptr<IAPIntSampler> WeightedAPIntSamplerSetBuilder::build() {
  return std::make_unique<WeightedAPIntSamplerSet>(WeightedSamplers);
}

APIntRangeSampler::APIntRangeSampler(APInt Min, APInt Max, bool IsSigned)
    : TheMin(std::move(Min)), TheMax(std::move(Max)), TheIsSigned(IsSigned) {
  assert((!IsSigned && TheMin.ule(TheMax)) || TheMin.sle(TheMax));
  assert(TheMin.getBitWidth() == TheMax.getBitWidth());
  // (NOTE): Implementing this for larger values would require
  // a thorough rewrite of UniformIntDistribution to support APInt natively.
  assert(TheMin.getBitWidth() <= CHAR_BIT * sizeof(LargestUnsignedNativeType) &&
         "APIntRangeSampler is not implemented for values wider than 64 bits");
}

Expected<APIntRangeSampler> APIntRangeSampler::create(APInt Min, APInt Max,
                                                      bool IsSigned) {
  auto CommonBitLength = std::max(Min.getBitWidth(), Max.getBitWidth());

  if (!IsSigned) {
    Min = Min.zext(CommonBitLength);
    Max = Max.zext(CommonBitLength);

    if (Min.ugt(Max))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          Twine("Min should be no larger than Max"));
  } else {
    Min = Min.sext(CommonBitLength);
    Max = Max.sext(CommonBitLength);

    if (Min.sgt(Max))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          Twine("Min should be no larger than Max"));
  }

  if (auto MaxBitLength = CHAR_BIT * sizeof(LargestUnsignedNativeType);
      Min.getBitWidth() > MaxBitLength)
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        Twine("APIntRangeSampler is not implemented for values wider than ")
            .concat(Twine(MaxBitLength))
            .concat(" bits"));

  return APIntRangeSampler(Min, Max, IsSigned);
}

} // namespace llvm::snippy
