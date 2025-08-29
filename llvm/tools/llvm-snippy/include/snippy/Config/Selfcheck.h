//===-- SelfcheckMode.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/TargetConfigIface.h"

#pragma once

namespace llvm {
namespace snippy {

enum class SelfcheckMode {
  Code,
  CheckSum,
};

struct SelfcheckModeEnumOption
    : public snippy::EnumOptionMixin<SelfcheckModeEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(
        SelfcheckMode::Code, "code",
        "selfcheck reference values are materialized during runtime");
    Mapper.enumCase(
        SelfcheckMode::CheckSum, "checksum",
        "selfcheck reference values are tracking with checksum computation");
  }
};

struct SelfcheckConfig {
  SelfcheckMode Mode;
  unsigned Period;
  bool SelfcheckGV;
  std::unique_ptr<SelfcheckTargetConfigInterface> STC = nullptr;

  bool isSelfcheckSectionRequired() const {
    return isMemoryBasedSelfcheckModeEnabled();
  }

  bool isCheckSumSelfcheckModeEnabled() const {
    return Mode == SelfcheckMode::CheckSum;
  }

  bool isMemoryBasedSelfcheckModeEnabled() const {
    return Mode == SelfcheckMode::Code
        ;
  }

  SelfcheckConfig(SelfcheckMode Mode = SelfcheckMode::Code, unsigned Period = 1,
                  bool SelfcheckGV = false)
      : Mode(Mode), Period(Period),
        SelfcheckGV(SelfcheckGV) {
  }

  SelfcheckConfig(const SelfcheckConfig &Other)
      : Mode(Other.Mode), Period(Other.Period),
        SelfcheckGV(Other.SelfcheckGV), STC(Other.STC->clone()) {
  }

  SelfcheckConfig(SelfcheckConfig &&Other) = default;

  SelfcheckConfig &operator=(const SelfcheckConfig &Other) {
    if (this == &Other)
      return *this;

    SelfcheckConfig Tmp = Other;
    std::swap(*this, Tmp);
    return *this;
  }

  SelfcheckConfig &operator=(SelfcheckConfig &&Other) = default;
  ~SelfcheckConfig() = default;
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(snippy::SelfcheckMode);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::SelfcheckConfig);

} // namespace llvm
