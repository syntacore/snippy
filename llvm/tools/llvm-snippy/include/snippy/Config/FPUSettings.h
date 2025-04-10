//===-- FPUSettings.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_CONFIG_FPU_SETTINGS_H
#define LLVM_SNIPPY_INCLUDE_CONFIG_FPU_SETTINGS_H

#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Config/Valuegram.h"
#include "snippy/Support/APIntSampler.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/Error.h"

#include <unordered_map>

namespace llvm {
namespace snippy {

struct FloatOverwriteRange final {
  using ValueType = int64_t;
  ValueType Min = 0;
  ValueType Max = 0;
  RoundingMode RM = RoundingMode::NearestTiesToEven;
  double Weight = 1.0;
};

enum class FloatOverwriteMode {
  IF_ANY_OPERAND,
  IF_ALL_OPERANDS,
  IF_MODEL_DETECTED_NAN,
  DISABLED,
};

template <FloatOverwriteMode M>
constexpr StringLiteral FloatOverwriteModeName = "";

template <>
inline constexpr StringLiteral
    FloatOverwriteModeName<FloatOverwriteMode::IF_ANY_OPERAND> =
        "if-any-operand";
template <>
inline constexpr StringLiteral
    FloatOverwriteModeName<FloatOverwriteMode::IF_ALL_OPERANDS> =
        "if-all-operands";
template <>
inline constexpr StringLiteral
    FloatOverwriteModeName<FloatOverwriteMode::DISABLED> = "disabled";
template <>
inline constexpr StringLiteral
    FloatOverwriteModeName<FloatOverwriteMode::IF_MODEL_DETECTED_NAN> =
        "if-model-detected-nan";

struct FloatOverwriteValues final {
  Valuegram TheValuegram;
};

struct FloatOverwriteSettings final {
  std::optional<FloatOverwriteRange> IntegralRange;
  std::optional<FloatOverwriteValues> HalfValues;
  std::optional<FloatOverwriteValues> SingleValues;
  std::optional<FloatOverwriteValues> DoubleValues;
  FloatOverwriteMode Mode = FloatOverwriteMode::IF_ALL_OPERANDS;

  bool needsModel() const {
    return Mode == FloatOverwriteMode::IF_MODEL_DETECTED_NAN;
  }
};

struct FPUSettings final {
  FloatOverwriteSettings Overwrite;
  bool needsModel() const { return Overwrite.needsModel(); }
};

LLVM_SNIPPY_YAML_STRONG_TYPEDEF(RoundingMode, FPURoundingMode);

Expected<std::unique_ptr<IAPIntSampler>>
createFloatOverwriteValueSampler(const FloatOverwriteSettings &Settings,
                                 const fltSemantics &Semantics);

class FloatSemanticsSamplerHolder final {
public:
  FloatSemanticsSamplerHolder(FloatOverwriteSettings Cfg)
      : OverwriteSettings{std::move(Cfg)} {}

  Expected<IAPIntSampler &> getSamplerFor(const fltSemantics &Semantics);

private:
  FloatOverwriteSettings OverwriteSettings;
  std::unordered_map<const fltSemantics *, std::unique_ptr<IAPIntSampler>>
      FloatValueSamplerForSemantics;
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::FPUSettings);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::FloatOverwriteValues);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::FloatOverwriteSettings);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::FloatOverwriteRange);

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::FloatOverwriteMode);
LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::FPURoundingMode);

} // namespace llvm

#endif
