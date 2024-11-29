//===-- GeneratorContext.h -------  -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/CallGraphLayout.h"
#include "snippy/Config/FPUSettings.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/BurstMode.h"
#include "snippy/Generator/GeneratorSettings.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/RegisterGenerator.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/SimRunner.h"
#include "snippy/Generator/SnippyModule.h"
#include "snippy/Generator/TopMemAccSampler.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/Debug.h"
namespace llvm {
class MachineModuleInfo;
class TargetSubtargetInfo;
} // namespace llvm

namespace llvm {
namespace snippy {
class ImmediateHistogram;
class InitialReg;
class Interpreter;
class LLVMState;
class MemoryScheme;
class OpcodeCache;
class RootRegPoolWrapper;
struct TargetGenContextInterface;
class Linker;
class GlobalsPool;
struct IRegisterState;

enum class GenerationMode {
  // Ignore Size requirements, only num Instrs
  NumInstrs,
  // Ignore num instrs, try to meet size requirements
  Size,
  // Try to satisfy both num instrs and size requirements
  Mixed
};

enum class LoopType { UpCount, DownCount };

using BurstGroupAccessDesc = std::vector<AddressInfo>;
using PlainAccessesType = std::vector<AccessAddress>;
using BurstGroupAccessesType = std::vector<BurstGroupAccessDesc>;

class GeneratorContext {
private:
  SnippyProgramContext &ProgContext;
  SnippyModule MainModule;

  GeneratorSettings *GenSettings = nullptr;

  std::unique_ptr<TargetGenContextInterface> TargetContext;

  TopLevelMemoryAccessSampler MemAccSampler;

  std::optional<FloatSemanticsSamplerHolder> FloatOverwriteSamplers;

  void diagnoseSelfcheckSection(size_t MinSize) const;

public:
  GeneratorContext(SnippyProgramContext &ProgContext,
                   GeneratorSettings &GenSettings);
  ~GeneratorContext();

  const auto &getProgramContext() const { return ProgContext; }

  auto &getProgramContext() { return ProgContext; }

  auto &getLLVMState() const { return getProgramContext().getLLVMState(); }

  auto &getLinker() { return getProgramContext().getLinker(); }
  const auto &getLinker() const { return getProgramContext().getLinker(); }

  auto &getMemoryAccessSampler() { return MemAccSampler; }

  void
  attachTargetContext(std::unique_ptr<TargetGenContextInterface> TgtContext);

  StringRef getLastInstr() const {
    return GenSettings->InstrsGenerationConfig.LastInstr;
  }
  bool useRetAsLastInstr() const {
    return StringRef{"RET"}.equals_insensitive(
        GenSettings->InstrsGenerationConfig.LastInstr);
  }
  StringRef getABIName() const { return GenSettings->ABIName; }

  auto getRequestedInstrsNumForMainFunction() const {
    return GenSettings->InstrsGenerationConfig.NumInstrs.value_or(0);
  }

  bool isLoopGenerationPossible() const {
    assert(GenSettings);
    const auto &Branches = GenSettings->Cfg.Branches;
    const auto &Histogram = GenSettings->Cfg.Histogram;
    return Branches.LoopRatio > std::numeric_limits<double>::epsilon() &&
           Branches.PermuteCF && Branches.MaxDepth.Loop > 0 &&
           Histogram.hasCFInstrs(ProgContext.getOpcodeCache());
  }

  bool isInstrsNumKnown() const {
    return GenSettings->InstrsGenerationConfig.NumInstrs.has_value();
  }
  const BurstGramData &getBurstGram() const {
    return *GenSettings->Cfg.Burst.Data;
  }

  auto &getConfig() const {
    assert(GenSettings);
    return GenSettings->Cfg;
  }

  GenerationMode getGenerationMode() const {
    assert((!isApplyValuegramEachInstr() || isInstrsNumKnown()) &&
           "Initialization of registers before each instruction is supported "
           "only if a number of instructions are generated.");
    if (isApplyValuegramEachInstr())
      return GenerationMode::NumInstrs;
    if (!isInstrsNumKnown())
      return GenerationMode::Size;
    bool PCDistanceRequested = getConfig().Branches.isPCDistanceRequested();
    return PCDistanceRequested ? GenerationMode::Mixed
                               : GenerationMode::NumInstrs;
  }

  auto &getSelfcheckSection() const {
    return ProgContext.getSelfcheckSection();
  }

  auto &getMemoryScheme() { return GenSettings->Cfg.MS; }
  const auto &getGenSettings() const {
    assert(GenSettings);
    return *GenSettings;
  }
  const auto &getCallGraphLayout() const { return GenSettings->Cfg.CGLayout; }

  template <typename It> size_t getCodeBlockSize(It Begin, It End) const {
    auto SizeAccumulator = [this](auto CurrSize, auto &MI) {
      size_t InstrSize =
          getLLVMState().getSnippyTarget().getInstrSize(MI, *this);
      if (InstrSize == 0)
        snippy::warn(
            WarningName::InstructionSizeUnknown,
            getProgramContext().getLLVMState().getCtx(),
            [&MI]() {
              std::string Ret;
              llvm::raw_string_ostream OS{Ret};
              OS << "Instruction '";
              MI.print(OS, /* IsStandalone */ true, /* SkipOpers */ true,
                       /* SkipDebugLoc */ true, /* AddNewLine */ false);
              OS << "' has unknown size";
              return Ret;
            }(),
            "function size estimation may be wrong");
      return CurrSize + InstrSize;
    };
    return std::accumulate(Begin, End, 0u, SizeAccumulator);
  }

  size_t getMBBSize(const MachineBasicBlock &MBB) const {
    return getCodeBlockSize(MBB.begin(), MBB.end());
  }

  size_t getFunctionSize(const MachineFunction &MF) const {
    return std::accumulate(MF.begin(), MF.end(), 0ul,
                           [this](auto CurrentSize, const auto &MBB) {
                             return CurrentSize + getMBBSize(MBB);
                           });
  }

  using OpcodeFilter = std::function<bool(unsigned)>;
  // FIXME: Those should be GeneratorContext's methods
  std::unique_ptr<DefaultOpcodeGenerator> createDefaultOpcodeGenerator() const {
    return GenSettings->Cfg.createDefaultOpcodeGenerator();
  }

  OpcGenHolder createCFOpcodeGenerator() const {
    return GenSettings->Cfg.createCFOpcodeGenerator(
        getProgramContext().getOpcodeCache());
  }

  OpcGenHolder createFlowOpcodeGenerator(
      OpcodeFilter OpcMask, bool MustHavePrimaryInstrs,
      ArrayRef<OpcodeHistogramEntry> Overrides,
      const std::unordered_map<unsigned, double> &WeightOverrides) const {
    return GenSettings->Cfg.createDFOpcodeGenerator(
        getProgramContext().getOpcodeCache(), OpcMask, Overrides,
        MustHavePrimaryInstrs, WeightOverrides);
  }

  auto getCFInstrsNum(size_t TotalInstructions) const {
    return GenSettings->Cfg.Histogram.getCFInstrsNum(
        TotalInstructions, getProgramContext().getOpcodeCache());
  }

  bool hasCallInstrs() const;

  bool hasCFInstrs() const {
    return GenSettings->Cfg.Histogram.hasCFInstrs(ProgContext.getOpcodeCache());
  }

  bool isApplyValuegramEachInstr() const {
    return GenSettings->Cfg.RegsHistograms.has_value();
  }

  TargetGenContextInterface &getTargetContext() const { return *TargetContext; }

  const auto &getMainModule() const { return MainModule; }

  auto &getMainModule() { return MainModule; }

  const TargetSubtargetInfo &getSubtargetImpl() const {
    auto &M = getMainModule().getModule();
    auto ModuleIt = M.begin();
    assert(ModuleIt != M.end() && "module must have at least one function");
    const Function &Fn = *ModuleIt;
    return *getLLVMState().getTargetMachine().getSubtargetImpl(Fn);
  }

  template <typename SubtargetType> const SubtargetType &getSubtarget() const {
    return static_cast<const SubtargetType &>(getSubtargetImpl());
  }

  ArrayRef<MCRegister> getRegsSpilledToStack() const {
    return GenSettings->RegistersConfig.SpilledToStack;
  }

  ArrayRef<MCRegister> getRegsSpilledToMem() const {
    return GenSettings->RegistersConfig.SpilledToMem;
  }
  bool isRegSpilledToMem(MCRegister Reg) const {
    return llvm::is_contained(getRegsSpilledToMem(), Reg);
  }

  // (FIXME): This should be moved to a less global context, when
  // GeneratorContext is finally split into more manageable pieces.
  IAPIntSampler &
  getOrCreateFloatOverwriteValueSampler(const fltSemantics &Semantics) {
    auto &FPUCfg = GenSettings->Cfg.FPUConfig;
    assert(FPUCfg && FPUCfg->Overwrite);
    assert(FloatOverwriteSamplers.has_value());
    auto SamplerRefOrErr = FloatOverwriteSamplers->getSamplerFor(Semantics);
    if (auto Err = SamplerRefOrErr.takeError())
      snippy::fatal(getLLVMState().getCtx(), "Internal error", std::move(Err));
    return *SamplerRefOrErr;
  }

  planning::GenPolicy
  createGenPolicy(const MachineBasicBlock &MBB,
                  std::unordered_map<unsigned, double> = {}) const;
};

} // namespace snippy
} // namespace llvm
