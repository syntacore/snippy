//===-- Config.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/CallGraphLayout.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Config/FPUSettings.h"
#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/PluginWrapper.h"
#include "snippy/Config/SelfcheckMode.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/TargetConfigIface.h"

#include "llvm/ADT/SmallSet.h"

#include <unordered_map>

namespace llvm {
namespace snippy {
#define GEN_SNIPPY_OPTIONS_STRUCT_DEF
#include "SnippyConfigOptionsStruct.inc"
#undef GEN_SNIPPY_OPTIONS_STRUCT_DEF
class SnippyTarget;

// Basic snippy configuration.
class ProgramConfig {
public:
  constexpr static auto SCStride = 16u;
  constexpr static auto kPageSize = 0x1000u;

  SectionsDescriptions Sections;
  std::unique_ptr<TargetConfigInterface> TargetConfig;
  std::unique_ptr<PluginManager> PluginManagerImpl;
  uint64_t Seed;

  // stack frame specific.
  std::string ABIName;
  MCRegister StackPointer;
  bool FollowTargetABI;
  SmallVector<MCRegister> SpilledToStack;
  SmallVector<MCRegister> SpilledToMem;
  bool ExternalStack;

  // linker options.
  bool MangleExportedNames;
  std::string EntryPointName;

  // TODO: rethink if it is needed here.
  std::string InitialRegYamlFile;

  ArrayRef<MCRegister> getRegsSpilledToStack() const { return SpilledToStack; }

  ArrayRef<MCRegister> getRegsSpilledToMem() const { return SpilledToMem; }

  bool isRegSpilledToMem(MCRegister Reg) const {
    return llvm::is_contained(SpilledToMem, Reg);
  }

  static constexpr unsigned getSCStride() { return SCStride; }
  static constexpr unsigned getPageSize() { return kPageSize; }

  ProgramConfig(const SnippyTarget &Tgt, StringRef PluginFilename);

  bool hasInternalStackSection() const {
    return Sections.hasSection(SectionsDescriptions::StackSectionName);
  }

  bool hasSectionToSpillGlobalRegs() const {
    return Sections.hasSection(SectionsDescriptions::UtilitySectionName);
  }
  bool stackEnabled() const {
    return ExternalStack ||
           Sections.hasSection(SectionsDescriptions::StackSectionName);
  }
};

struct TrackingOptions {
  bool BTMode;
  unsigned SelfCheckPeriod;
  bool AddressVH;
};

// Settings common for all policies there are.
class CommonPolicyConfig {
public:
  const ProgramConfig &ProgramCfg;
  MemoryScheme MS;
  ImmediateHistogram ImmHistogram;
  FPUSettings FPUConfig;
  OpcodeToImmHistSequenceMap ImmHistMap;
  TrackingOptions TrackCfg;

  CommonPolicyConfig(const ProgramConfig &ProgramCfg)
      : ProgramCfg(ProgramCfg) {}

  void setupImmHistMap(const OpcodeCache &OpCC, const OpcodeHistogram &OpHist) {
    if (!ImmHistogram.holdsAlternative<ImmediateHistogramRegEx>())
      return;
    ImmHistMap = OpcodeToImmHistSequenceMap(
        ImmHistogram.get<ImmediateHistogramRegEx>(), OpHist, OpCC);
  }
};

class DefaultPolicyConfig {
public:
  const CommonPolicyConfig &Common;
  OpcodeHistogram DataFlowHistogram;
  struct ValuegramOpt {
    RegistersWithHistograms RegsHistograms;
    bool ValuegramOperandsRegsInitOutputs;
  };
  std::optional<ValuegramOpt> Valuegram;

  DefaultPolicyConfig(const CommonPolicyConfig &Common) : Common(Common) {}

  OpcGenHolder createOpcodeGenerator(
      const OpcodeCache &OpCC, std::function<bool(unsigned)> OpcMask,
      ArrayRef<OpcodeHistogramEntry> Overrides, bool MustHavePrimaryInstrs,
      std::unordered_map<unsigned, double> OpcWeightOverrides = {}) const {

    std::map<unsigned, double> DFHCopy;
    llvm::copy_if(DataFlowHistogram, std::inserter(DFHCopy, DFHCopy.end()),
                  [&](auto &&Entry) { return OpcMask(Entry.first); });
    if (MustHavePrimaryInstrs && DFHCopy.size() == 0)
      snippy::fatal(
          "We can not create any primary instruction in this context.\nUsually "
          "this may happen when in some context snippy can not find any "
          "instruction that could be created in current context.\nTry to "
          "increase instruction number by one or add more instructions to "
          "histogram.");
    // overriding previous weights
    if (!OpcWeightOverrides.empty()) {
      for (auto &&[Opcode, Weight] : OpcWeightOverrides) {
        if (DFHCopy.count(Opcode))
          DFHCopy[Opcode] = Weight;
      }
    }

    for (const auto &Entry : Overrides)
      if (!Entry.deactivated())
        DFHCopy[Entry.Opcode] = Entry.Weight;
    auto &PluginManager = *Common.ProgramCfg.PluginManagerImpl;
    if (PluginManager.pluginHasBeenLoaded())
      return PluginManager.createPlugin(DFHCopy.begin(), DFHCopy.end());
    return std::make_unique<DefaultOpcodeGenerator>(DFHCopy.begin(),
                                                    DFHCopy.end());
  }

  bool isApplyValuegramEachInstr() const { return Valuegram.has_value(); }
};

class BurstPolicyConfig {
public:
  const CommonPolicyConfig &Common;
  BurstGramData Burst;
  std::unordered_map<unsigned, double> BurstOpcodeWeights;

  BurstPolicyConfig(const CommonPolicyConfig &Common) : Common(Common) {}
};

struct ModelPluginOptions {
  std::vector<std::string> ModelLibraries;

  bool runOnModel() const { return !ModelLibraries.empty(); }
};

struct InstrsGenerationOptions {
  bool RunMachineInstrVerifier;
  bool ChainedRXSectionsFill;
  bool ChainedRXSorted;
  std::optional<unsigned> ChainedRXChunkSize;
  std::optional<unsigned> NumInstrs;
  std::string LastInstr;
  bool useRetAsLastInstr() const {
    return StringRef{"RET"}.equals_insensitive(LastInstr);
  }
  auto getRequestedInstrsNumForMainFunction() const {
    return NumInstrs.value_or(0);
  }
  bool isInstrsNumKnown() const { return NumInstrs.has_value(); }
};

struct RegistersOptions {
  bool InitializeRegs;
  // TODO: discuss these to be Interpreter-only options
  std::string InitialStateOutputYaml;
  std::string FinalStateOutputYaml;
};

// Settings specific for pass behaviour.
class PassConfig {
public:
  const ProgramConfig &ProgramCfg;

  // CF generator passes configuration.
  Branchegram Branches;
  OpcodeHistogram BranchOpcodes;
  ModelPluginOptions ModelPluginConfig;
  InstrsGenerationOptions InstrsGenerationConfig;
  RegistersOptions RegistersConfig;

  // Function generator pass config.
  std::variant<CallGraphLayout, FunctionDescs> CGLayout;

  PassConfig(const ProgramConfig &ProgramCfg) : ProgramCfg(ProgramCfg) {}

  OpcGenHolder createCFOpcodeGenerator(const OpcodeCache &OpCC) const {
    auto &PluginManager = *ProgramCfg.PluginManagerImpl;
    if (PluginManager.pluginHasBeenLoaded())
      return PluginManager.createPlugin(BranchOpcodes.begin(),
                                        BranchOpcodes.end());
    return std::make_unique<DefaultOpcodeGenerator>(BranchOpcodes.begin(),
                                                    BranchOpcodes.end());
  }

  bool hasExternalCallees() const {
    if (!std::holds_alternative<FunctionDescs>(CGLayout))
      return false;
    auto &FuncDescs = std::get<FunctionDescs>(CGLayout);
    return llvm::any_of(FuncDescs.Descs, [&](auto &Func) {
      return hasExternalCallee(FuncDescs, Func);
    });
  }
};

enum class GenerationMode {
  // Ignore Size requirements, only num Instrs
  NumInstrs,
  // Ignore num instrs, try to meet size requirements
  Size,
  // Try to satisfy both num instrs and size requirements
  Mixed
};

class IncludePreprocessor final {
public:
  // NOTE: Lifetime of FileName field is the same as the
  // lifetime of the enclosing class.
  // Care needs to be taken that this StringRef does not dangle.
  struct LineID final {
    StringRef FileName;
    unsigned N;
  };

private:
  std::string PrimaryFilename;
  std::string Text;
  std::vector<LineID> Lines;
  SmallSet<std::string, 8> IncludedFiles;

public:
  IncludePreprocessor(StringRef Filename,
                      const std::vector<std::string> &IncludeDirs,
                      LLVMContext &Ctx);

  void mergeFile(StringRef FileName, StringRef Contents);
  LineID getCorrespondingLineID(unsigned GlobalID) const & {
    assert(GlobalID > 0 && GlobalID <= Lines.size());
    return Lines[GlobalID - 1];
  }

  StringRef getPreprocessed() const & { return Text; }
  auto getIncludes() const & {
    return llvm::make_range(IncludedFiles.begin(), IncludedFiles.end());
  }
  StringRef getPrimaryFilename() const & { return PrimaryFilename; }
};

// legacy config
class Config final {
public:
  using OpcodeFilter = std::function<bool(unsigned)>;

  // legacy specific.
  std::vector<std::string> Includes;
  std::unique_ptr<ProgramConfig> ProgramCfg;

  // Top-level histogram.
  OpcodeHistogram Histogram;

  // Policies.
  std::unique_ptr<CommonPolicyConfig> CommonPolicyCfg;
  DefaultPolicyConfig DefFlowConfig;
  std::optional<BurstPolicyConfig> BurstConfig;
  // std::optional<ValuegramPolicyConfig> ValuegramConfig;
  PassConfig PassCfg;

  Config(IncludePreprocessor &IPP, RegPoolWrapper &RP, LLVMState &State

         ,
         StringRef PluginFilename, StringRef PluginInfoFilename,
         const OpcodeCache &OpCC, bool ParseWithPlugin);

  // FIXME: legacy that must be removed
  // FIXME: this should return OpcGenHolder
  std::unique_ptr<DefaultOpcodeGenerator> createDefaultOpcodeGenerator() const {
    return std::make_unique<DefaultOpcodeGenerator>(Histogram.begin(),
                                                    Histogram.end());
  }

  double getBurstOpcodesWeight() const {
    if (!BurstConfig)
      return 0.0;
    auto &BCfg = *BurstConfig;
    auto BurstOpcodes = BCfg.Burst.getAllBurstOpcodes();
    return Histogram.getOpcodesWeight([&BurstOpcodes](unsigned Opcode) {
      return BurstOpcodes.count(Opcode);
    });
  }

  bool isLoopGenerationPossible(const OpcodeCache &OpCC) const {
    const auto &Branches = PassCfg.Branches;
    return Branches.LoopRatio > std::numeric_limits<double>::epsilon() &&
           Branches.PermuteCF && Branches.MaxDepth.Loop > 0 &&
           Histogram.hasCFInstrs(OpCC);
  }

  GenerationMode getGenerationMode() const {
    assert((!DefFlowConfig.isApplyValuegramEachInstr() ||
            PassCfg.InstrsGenerationConfig.isInstrsNumKnown()) &&
           "Initialization of registers before each instruction is supported "
           "only if a number of instructions are generated.");
    if (DefFlowConfig.isApplyValuegramEachInstr())
      return GenerationMode::NumInstrs;
    if (!PassCfg.InstrsGenerationConfig.isInstrsNumKnown())
      return GenerationMode::Size;
    bool PCDistanceRequested = PassCfg.Branches.isPCDistanceRequested();
    return PCDistanceRequested ? GenerationMode::Mixed
                               : GenerationMode::NumInstrs;
  }

  auto getCFInstrsNum(const OpcodeCache &OpCC, size_t TotalInstructions) const {
    return Histogram.getCFInstrsNum(TotalInstructions, OpCC);
  }

  bool hasCallInstrs(const OpcodeCache &OpCC, const SnippyTarget &Tgt) const {
    return Histogram.hasCallInstrs(OpCC, Tgt);
  }

  bool hasCFInstrs(const OpcodeCache &OpCC) const {
    return Histogram.hasCFInstrs(OpCC);
  }

  auto &getTrackCfg() const { return CommonPolicyCfg->TrackCfg; }

  bool hasTrackingMode() const {
    return getTrackCfg().BTMode || getTrackCfg().SelfCheckPeriod ||
           getTrackCfg().AddressVH ||

           CommonPolicyCfg->FPUConfig.needsModel();
  }

  void dump(raw_ostream &OS, const ConfigIOContext &Ctx) const;

private:
  void complete(LLVMState &State, const OpcodeCache &OpCC);
  void validateAll(LLVMState &State, const OpcodeCache &cache,
                   const RegPoolWrapper &RP);
};

bool shouldSpillGlobalRegs(const Config &Cfg);

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS_WITH_VALIDATE(snippy::Config);
} // namespace llvm
