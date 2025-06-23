//===-- SelfcheckMode.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLUtils.h"

#pragma once

namespace llvm {
namespace snippy {

enum class SelfcheckRefValueStorageType {
  Code,
};

struct SelfcheckRefValueStorageEnumOption
    : public snippy::EnumOptionMixin<SelfcheckRefValueStorageEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(
        SelfcheckRefValueStorageType::Code, "code",
        "selfcheck reference values are materialized during runtime");
  }
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_SCALAR_ENUMERATION_TRAITS(
    snippy::SelfcheckRefValueStorageType);

} // namespace llvm
