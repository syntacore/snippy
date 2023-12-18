//===-- FunctionDescriptions.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/YAMLUtils.h"

#include <string>
#include <vector>

namespace llvm {
namespace snippy {

struct FunctionDesc {
  std::string Name;
  bool External = false;
  std::vector<std::string> Callees;
};

struct FunctionDescs {
  std::string EntryPoint;
  std::vector<FunctionDesc> Descs;
  auto getEntryPointDesc() const {
    return std::find_if(Descs.begin(), Descs.end(),
                        [this](auto &Desc) { return Desc.Name == EntryPoint; });
  }
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::FunctionDesc);
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::FunctionDescs);
LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(snippy::FunctionDesc, false);
} // namespace llvm
