//===-- APIntSampler.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <type_traits>
#include <vector>

namespace llvm {
namespace snippy {

// (NOTE): Ideally this would not depend on global context and
// snippy::RandEngine, but alas, it's too deeply ingrained in the current code
// and ripping it out would be a huge refactor. At some point we really do need
// to create a separate random engine entity, which is not global.
class IAPIntSampler {
public:
  virtual Expected<APInt> sample() = 0;
  virtual ~IAPIntSampler() = default;
};

class ConstantAPIntSampler : public IAPIntSampler {
public:
  explicit ConstantAPIntSampler(llvm::APInt Val) : TheValue(std::move(Val)) {}
  Expected<APInt> sample() override { return TheValue; }

private:
  APInt TheValue;
};

class APIntRangeSampler : public IAPIntSampler {
protected:
  using LargestUnsignedNativeType =
      decltype(std::declval<APInt>().getZExtValue());
  using LargestSignedNativeType =
      decltype(std::declval<APInt>().getSExtValue());

  auto getBitWidth() const { return TheMin.getBitWidth(); }

public:
  explicit APIntRangeSampler(APInt Min, APInt Max, bool IsSigned = false);
  static Expected<APIntRangeSampler> create(APInt Min, APInt Max,
                                            bool IsSigned = false);

  Expected<APInt> sample() override;

private:
  APInt TheMin;
  APInt TheMax;
  bool TheIsSigned;
};

class BitPatternAPIntSamler : public IAPIntSampler {
public:
  BitPatternAPIntSamler(uint32_t NumBits) : TheNumBits{NumBits} {}
  auto getNumBits() const noexcept { return TheNumBits; }
  Expected<APInt> sample() override { return generate(getNumBits()); }
  static APInt generate(uint32_t NumBits);

private:
  uint32_t TheNumBits;
};

class UniformAPIntSamler : public IAPIntSampler {
public:
  UniformAPIntSamler(uint32_t NumBits) : TheNumBits{NumBits} {}
  auto getNumBits() const noexcept { return TheNumBits; }
  Expected<APInt> sample() override { return generate(getNumBits()); }
  static APInt generate(uint32_t NumBits);

private:
  uint32_t TheNumBits;
};

template <typename ISampler = IAPIntSampler>
class WeightedAPIntSamplerSetBuilder {
  struct WeightedSamplerT {
    WeightedSamplerT(std::unique_ptr<ISampler> SamplerParam, double WeightParam)
        : Sampler(std::move(SamplerParam)), Weight(WeightParam) {}
    std::unique_ptr<ISampler> Sampler;
    double Weight;
  };

public:
  WeightedAPIntSamplerSetBuilder() = default;

  template <typename SamplerT,
            typename = std::enable_if_t<std::is_base_of_v<ISampler, SamplerT>>>
  void addOwned(SamplerT Sampler, double Weight) {
    WeightedSamplers.emplace_back(
        /*Sampler=*/std::make_unique<SamplerT>(std::move(Sampler)),
        /*Weight=*/Weight);
  }

  void addOwned(std::unique_ptr<ISampler> Sampler, double Weight) {
    WeightedSamplers.emplace_back(
        /*Sampler=*/std::move(Sampler),
        /*Weight=*/Weight);
  }

  bool isEmpty() const { return WeightedSamplers.empty(); }

  std::unique_ptr<ISampler> build();

private:
  std::vector<WeightedSamplerT> WeightedSamplers;
};

} // namespace snippy
} // namespace llvm
