//===-- APIntSampler.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_APINTSAMPLER_H
#define LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_APINTSAMPLER_H

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <type_traits>
#include <vector>

namespace llvm {
namespace snippy {

struct APIntWithSign {
  APInt Value;
  /// Whether the specified value is negative. Useful for callers that need to
  /// decide when to zero or sign extend the value depending on the context.
  bool IsSigned;
  static Expected<APInt> parseAPInt(StringRef StrView, bool HasNegativeSign,
                                    unsigned Radix, StringRef OriginalStr);
  static Error reportError(Twine Msg);

  static Expected<APFloat> parseFPFmtAPInt(StringRef &StrView,
                                           bool HasNegativeSign,
                                           StringRef OriginalStr);
};

// (NOTE): Ideally this would not depend on global context and
// snippy::RandEngine, but alas, it's too deeply ingrained in the current code
// and ripping it out would be a huge refactor. At some point we really do need
// to create a separate random engine entity, which is not global.
class IAPIntSampler {
public:
  virtual Expected<APIntWithSign> sample() = 0;
  virtual ~IAPIntSampler() = default;
};

class ConstantAPIntSampler : public IAPIntSampler {
public:
  explicit ConstantAPIntSampler(APIntWithSign Val) : TheValue(std::move(Val)) {}
  Expected<APIntWithSign> sample() override { return TheValue; }

private:
  APIntWithSign TheValue;
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

  Expected<APIntWithSign> sample() override;

private:
  APInt TheMin;
  APInt TheMax;
  bool TheIsSigned;
};

class BitPatternAPIntSampler : public IAPIntSampler {
public:
  BitPatternAPIntSampler(uint32_t NumBits) : TheNumBits{NumBits} {}
  auto getNumBits() const noexcept { return TheNumBits; }
  Expected<APIntWithSign> sample() override { return generate(getNumBits()); }
  static APIntWithSign generate(uint32_t NumBits);

private:
  uint32_t TheNumBits;
};

class UniformAPIntSampler : public IAPIntSampler {
public:
  UniformAPIntSampler(uint32_t NumBits) : TheNumBits{NumBits} {}
  auto getNumBits() const noexcept { return TheNumBits; }
  Expected<APIntWithSign> sample() override { return generate(getNumBits()); }
  static APIntWithSign generate(uint32_t NumBits);

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
#endif // LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_APINTSAMPLER_H
