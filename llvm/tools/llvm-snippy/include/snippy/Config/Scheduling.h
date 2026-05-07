//===-- Scheduling.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Simulator/Types.h"
#include "snippy/Support/YAMLUtils.h"

namespace llvm {
namespace snippy {

struct SchedulingSettings final {
  SchedulingSettings();

  // 2^17 = 131072 - Seems to be an ok default that does not lead to entirely
  // unreasonable run time for large tests. For each configuration the optimal
  // value might be very far off from this and might require some
  // trial-and-error.
  static constexpr uint64_t kDefaultMaxRegionSize = 1ull << 17;
  static constexpr bool kEnabledByDefault = false;

  std::optional<bool> Enabled = std::nullopt;
  uint64_t MaxRegionSize = kDefaultMaxRegionSize;

  bool isEnabled() const { return Enabled.value_or(kEnabledByDefault); }
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(
    snippy::SchedulingSettings);

} // namespace llvm
