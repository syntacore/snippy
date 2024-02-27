//===-- GeneratorSettings.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GENERATOR_SETTINGS_H
#define LLVM_TOOLS_SNIPPY_GENERATOR_SETTINGS_H

#include "snippy/Config/Config.h"
#include "snippy/Generator/BurstMode.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <string>

namespace llvm {
namespace snippy {

struct DebugOptions {
  bool PrintInstrs;
  bool PrintMachineFunctions;
  bool PrintControlFlowGraph;
};

struct TrackingOptions {
  bool BTMode;
  unsigned SelfCheckPeriod;
  bool AddressVH;
};

struct InstrsGenerationOptions {
  bool RunMachineInstrVerifier;
  bool ChainedRXSectionsFill;
  bool ChainedRXSorted;
  std::optional<unsigned> NumInstrs;
  std::string LastInstr;
};

struct LinkerOptions {
  bool ExternalStack;
  bool MangleExportedNames;
  std::string EntryPointName;
};

struct ModelPluginOptions {
  bool RunOnModel;
  std::vector<std::string> ModelLibraries;
};

struct RegistersOptions {
  bool InitializeRegs;
  std::string InitialRegYamlFile;
  // TODO: discuss these to be Interpreter-only options
  std::string InitialStateOutputYaml;
  std::string FinalStateOutputYaml;
  SmallVector<unsigned> SpilledRegs;
};

struct GeneratorSettings {
  std::string ABIName;
  std::string BaseFileName;

  TrackingOptions TrackingConfig;
  DebugOptions DebugConfig;
  LinkerOptions LinkerConfig;
  ModelPluginOptions ModelPluginConfig;
  InstrsGenerationOptions InstrsGenerationConfig;
  RegistersOptions RegistersConfig;

  Config Cfg;

  GeneratorSettings(std::string ABIName, std::string BaseFileName,
                    TrackingOptions &&TrackingConfig,
                    DebugOptions &&DebugConfig, LinkerOptions &&LinkerConfig,
                    ModelPluginOptions &&ModelPluginConfig,
                    InstrsGenerationOptions &&InstrsGenerationConfig,
                    RegistersOptions &&RegistersConfig, Config &&Cfg)
      : ABIName(std::move(ABIName)), BaseFileName(std::move(BaseFileName)),
        TrackingConfig(std::move(TrackingConfig)),
        DebugConfig(std::move(DebugConfig)),
        LinkerConfig(std::move(LinkerConfig)),
        ModelPluginConfig(std::move(ModelPluginConfig)),
        InstrsGenerationConfig(std::move(InstrsGenerationConfig)),
        RegistersConfig(std::move(RegistersConfig)), Cfg(std::move(Cfg)) {}
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_GENERATOR_SETTINGS_H
