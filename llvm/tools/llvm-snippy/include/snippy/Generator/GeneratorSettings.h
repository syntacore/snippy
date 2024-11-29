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
#include "snippy/Generator/Policy.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <string>

namespace llvm {
namespace snippy {

struct DebugOptions {
  bool PrintInstrs;
  bool PrintMachineFunctions;
  bool PrintControlFlowGraph;
  bool ViewControlFlowGraph;
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
  std::optional<unsigned> ChainedRXChunkSize;
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
  bool FollowTargetABI;
  std::string InitialRegYamlFile;
  // TODO: discuss these to be Interpreter-only options
  std::string InitialStateOutputYaml;
  std::string FinalStateOutputYaml;
  SmallVector<MCRegister> SpilledToStack;
  SmallVector<MCRegister> SpilledToMem;
  MCRegister StackPointer;
};

struct SnippyProgramSettings {
  SectionsDescriptions Sections;
  MCRegister StackPointer;
  bool MangleExportedNames;
  bool FollowTargetABI;
  bool ExternalStack;
  std::string EntryPointName;
  std::string InitialRegYamlFile;

  SnippyProgramSettings(SectionsDescriptions Sections, MCRegister StackPointer,
                        bool MangleExportedNames, bool FollowTargetABI,
                        bool ExternalStack, StringRef EntryPointName,
                        StringRef InitialRegYamlFile)
      : Sections(std::move(Sections)), StackPointer(StackPointer),
        MangleExportedNames(MangleExportedNames),
        FollowTargetABI(FollowTargetABI), ExternalStack(ExternalStack),
        EntryPointName(EntryPointName),
        InitialRegYamlFile(InitialRegYamlFile){};
};

class GeneratorSettings {
public:
  std::string ABIName;
  std::string BaseFileName;
  std::string LayoutFile;
  std::vector<std::string> AdditionalLayoutFiles;
  TrackingOptions TrackingConfig;
  DebugOptions DebugConfig;
  LinkerOptions LinkerConfig;
  ModelPluginOptions ModelPluginConfig;
  InstrsGenerationOptions InstrsGenerationConfig;
  RegistersOptions RegistersConfig;

  Config Cfg;

  SnippyProgramSettings getSnippyProgramSettings(LLVMState &State) const {
    return SnippyProgramSettings(
        getCompleteSectionList(State), RegistersConfig.StackPointer,
        LinkerConfig.MangleExportedNames, RegistersConfig.FollowTargetABI,
        LinkerConfig.ExternalStack, LinkerConfig.EntryPointName,
        RegistersConfig.InitialRegYamlFile);
  }

  GeneratorSettings(std::string ABIName, std::string BaseFileName,
                    std::string LayoutFile,
                    const std::vector<std::string> &AdditionalLayoutFiles,
                    TrackingOptions &&TrackingConfig,
                    DebugOptions &&DebugConfig, LinkerOptions &&LinkerConfig,
                    ModelPluginOptions &&ModelPluginConfig,
                    InstrsGenerationOptions &&InstrsGenerationConfig,
                    RegistersOptions &&RegsConfig, Config &&Cfg)
      : ABIName(std::move(ABIName)), BaseFileName(std::move(BaseFileName)),
        LayoutFile(std::move(LayoutFile)),
        AdditionalLayoutFiles(AdditionalLayoutFiles),
        TrackingConfig(std::move(TrackingConfig)),
        DebugConfig(std::move(DebugConfig)),
        LinkerConfig(std::move(LinkerConfig)),
        ModelPluginConfig(std::move(ModelPluginConfig)),
        InstrsGenerationConfig(std::move(InstrsGenerationConfig)),
        RegistersConfig(std::move(RegsConfig)), Cfg(std::move(Cfg)) {}

private:
  SectionsDescriptions getCompleteSectionList(LLVMState &State) const;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_GENERATOR_SETTINGS_H
