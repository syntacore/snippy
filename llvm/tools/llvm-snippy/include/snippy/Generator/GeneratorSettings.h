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

enum class GenerationMode {
  // Ignore Size requirements, only num Instrs
  NumInstrs,
  // Ignore num instrs, try to meet size requirements
  Size,
  // Try to satisfy both num instrs and size requirements
  Mixed
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

  StringRef getLastInstr() const { return InstrsGenerationConfig.LastInstr; }
  bool useRetAsLastInstr() const {
    return StringRef{"RET"}.equals_insensitive(
        InstrsGenerationConfig.LastInstr);
  }
  StringRef getABIName() const { return ABIName; }

  auto getRequestedInstrsNumForMainFunction() const {
    return InstrsGenerationConfig.NumInstrs.value_or(0);
  }

  bool isLoopGenerationPossible(const OpcodeCache &OpCC) const {
    const auto &Branches = Cfg.Branches;
    const auto &Histogram = Cfg.Histogram;
    return Branches.LoopRatio > std::numeric_limits<double>::epsilon() &&
           Branches.PermuteCF && Branches.MaxDepth.Loop > 0 &&
           Histogram.hasCFInstrs(OpCC);
  }

  bool isInstrsNumKnown() const {
    return InstrsGenerationConfig.NumInstrs.has_value();
  }
  const BurstGramData &getBurstGram() const { return *Cfg.Burst.Data; }

  ArrayRef<MCRegister> getRegsSpilledToStack() const {
    return RegistersConfig.SpilledToStack;
  }

  ArrayRef<MCRegister> getRegsSpilledToMem() const {
    return RegistersConfig.SpilledToMem;
  }

  bool isRegSpilledToMem(MCRegister Reg) const {
    return llvm::is_contained(getRegsSpilledToMem(), Reg);
  }

  OpcGenHolder createCFOpcodeGenerator(const OpcodeCache &OpCC) const {
    return Cfg.createCFOpcodeGenerator(OpCC);
  }

  using OpcodeFilter = std::function<bool(unsigned)>;
  OpcGenHolder createFlowOpcodeGenerator(
      const OpcodeCache &OpCC, OpcodeFilter OpcMask, bool MustHavePrimaryInstrs,
      ArrayRef<OpcodeHistogramEntry> Overrides,
      const std::unordered_map<unsigned, double> &WeightOverrides) const {
    return Cfg.createDFOpcodeGenerator(OpCC, OpcMask, Overrides,
                                       MustHavePrimaryInstrs, WeightOverrides);
  }

  auto getCFInstrsNum(const OpcodeCache &OpCC, size_t TotalInstructions) const {
    return Cfg.Histogram.getCFInstrsNum(TotalInstructions, OpCC);
  }

  bool hasCallInstrs(const OpcodeCache &OpCC, const SnippyTarget &Tgt) const {
    return Cfg.Histogram.hasCallInstrs(OpCC, Tgt);
  }

  bool hasCFInstrs(const OpcodeCache &OpCC) const {
    return Cfg.Histogram.hasCFInstrs(OpCC);
  }

  bool isApplyValuegramEachInstr() const {
    return Cfg.RegsHistograms.has_value();
  }

  GenerationMode getGenerationMode() const {
    assert((!isApplyValuegramEachInstr() || isInstrsNumKnown()) &&
           "Initialization of registers before each instruction is supported "
           "only if a number of instructions are generated.");
    if (isApplyValuegramEachInstr())
      return GenerationMode::NumInstrs;
    if (!isInstrsNumKnown())
      return GenerationMode::Size;
    bool PCDistanceRequested = Cfg.Branches.isPCDistanceRequested();
    return PCDistanceRequested ? GenerationMode::Mixed
                               : GenerationMode::NumInstrs;
  }

  const auto &getCallGraphLayout() const { return Cfg.CGLayout; }

  std::unique_ptr<DefaultOpcodeGenerator> createDefaultOpcodeGenerator() const {
    return Cfg.createDefaultOpcodeGenerator();
  }

private:
  SectionsDescriptions getCompleteSectionList(LLVMState &State) const;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_GENERATOR_SETTINGS_H
