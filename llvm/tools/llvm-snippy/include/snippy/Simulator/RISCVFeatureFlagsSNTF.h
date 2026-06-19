//===-- RISCVFeatureFlagsSNTF.h------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_RISCVFEATUREFLAGSSNTF_H
#define LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_RISCVFEATUREFLAGSSNTF_H

#include "snippy/Support/YAMLUtils.h"

#include <tuple>

namespace llvm {
namespace snippy {

struct RISCVFeatureFlagsSNTF {
  bool EnableTime = true;
  bool EnablePC = true;
  bool EnableInstrCode = true;
  bool EnableNextPC = true;
  bool EnableRegVals = true;
  bool EnableMemAccesses = true;
  bool EnableCSRVals = false;

  bool operator==(const RISCVFeatureFlagsSNTF &Other) const {
    return std::tie(EnableTime, EnablePC, EnableInstrCode, EnableNextPC,
                    EnableRegVals, EnableMemAccesses, EnableCSRVals) ==
           std::tie(Other.EnableTime, Other.EnablePC, Other.EnableInstrCode,
                    Other.EnableNextPC, Other.EnableRegVals,
                    Other.EnableMemAccesses, Other.EnableCSRVals);
  }
};

} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::RISCVFeatureFlagsSNTF);

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_RISCVFEATUREFLAGSSNTF_H
