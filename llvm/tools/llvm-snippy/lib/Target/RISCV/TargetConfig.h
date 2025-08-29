#ifndef LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_CONFIG_H
#define LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_CONFIG_H

//===-- TargetConfig.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"

#include "RVVUnitConfig.h"
#include <memory>

namespace llvm::snippy {

class TargetConfigInterface;

class RISCVConfigInterface : public TargetConfigInterface {
public:
  void mapConfig(yaml::IO &IO) override {
    RVVConfig = createRVVConfig();
    RVVConfig->mapYaml(IO);
  }

  bool hasConfig() const override {
    if (RVVConfig)
      return RVVConfig->hasConfig();

    return false;
  }

  std::unique_ptr<RVVConfigInterface> RVVConfig;
};

struct RISCVSelfcheckTargetConfig final
    : public SelfcheckTargetConfigInterface {
  std::optional<bool> SelfcheckRVVEnabled;

  std::unique_ptr<SelfcheckTargetConfigInterface> clone() const override {
    return std::make_unique<RISCVSelfcheckTargetConfig>(*this);
  }

  void mapConfig(yaml::IO &IO) override {
    IO.mapOptional("enable-selfcheck-rvv", SelfcheckRVVEnabled);
  }
};

} // namespace llvm::snippy

#endif // LLVM_TOOLS_SNIPPY_LIB_RISCV_TARGET_CONFIG_H
