//===-- TargetConfig.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_AARCH64_TARGETCONFIG_H
#define LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_AARCH64_TARGETCONFIG_H

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Target/Target.h"

namespace llvm::snippy {

class TargetConfigInterface;

class AArch64ConfigInterface : public TargetConfigInterface {
public:
  void mapConfig(yaml::IO &IO) override {
    // TODO: Add here config in future
  }

  bool hasConfig() const override { return false; }
};

} // namespace llvm::snippy
#endif // LLVM_TOOLS_LLVM_SNIPPY_LIB_TARGET_AARCH64_TARGETCONFIG_H
