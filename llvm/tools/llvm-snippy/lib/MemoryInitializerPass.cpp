//===-- MemoryInitializerPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Generator/SnippyFunctionMetadata.h"
#include "MemoryInitializerPass.h"
#include "snippy/Config/MemInitializationMode.h"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

#define DEBUG_TYPE "snippy-memory-initializer"
#define PASS_DESC "Snippy Memory Initializer"

StringRef MemoryInitializer::getPassName() const { return PASS_DESC " Pass"; }
void MemoryInitializer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<SnippyFunctionMetadataWrapper>();
  AU.addRequired<SimulatorContextWrapper>();
  ModulePass::getAnalysisUsage(AU);
}

char MemoryInitializer::ID = 0;

} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::MemoryInitializer;

INITIALIZE_PASS_BEGIN(MemoryInitializer, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(MemoryInitializer, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createMemoryInitializerPass(bool ExternalCallOfMemInitRoutine) {
  return new MemoryInitializer(ExternalCallOfMemInitRoutine);
}

namespace snippy {
namespace {

void addSectionsToSnippyState(const MemoryManager &MemManager, Linker &Linker,
                              GeneratorContext &SGCtx,
                              SimulatorContext &SimCtx) {
  auto ReadWriteSections = make_filter_range(
      MemManager.getMemState(),
      [](const SectionData &SectData) { return !SectData.HasLaterInit; });
  for_each(ReadWriteSections, [&Linker](const SectionData &SectData) {
    Linker.sections().addInputSectionFor(SectData.Desc,
                                         SectData.Desc.getName());
  });
}

template <typename Init>
void processObjectFileInitFull(Module &M, MemoryManager &MemManager,
                               const Config &Cfg, Linker &Linker,
                               GeneratorContext &SGCtx,
                               SimulatorContext &SimCtx) {
  auto *EntryF = M.getFunction(Cfg.ProgramCfg.EntryPointName);
  assert(EntryF);
  auto *EntryMF =
      SnippyModule::fromModule(M).getMMI().getMachineFunction(*EntryF);
  assert(EntryMF && !EntryMF->empty());
  auto &MBB = EntryMF->front();
  auto Ins = MBB.begin();
  // FIXME: this IGC is dummy and should not be used oto generate
  // any instructions. However it's kept here, because getSectionData
  // uses it.
  InstructionGenerationContext InstrGenCtx{MBB, Ins, SGCtx};
  MemManager.materializeSectionData<Init>(M, InstrGenCtx,
                                          Cfg.ProgramCfg.MemoryCfg.MemorySeed);
  addSectionsToSnippyState(MemManager, Linker, SGCtx, SimCtx);
}

void processFullRuntimeInit(MemoryManager &MemManager, const Config &Cfg,
                            bool ExternalCallOfMemInitRoutine, Module &M,
                            GeneratorContext &SGCtx) {
  auto *EntryF = M.getFunction(Cfg.ProgramCfg.EntryPointName);
  assert(EntryF);
  auto *EntryMF =
      SnippyModule::fromModule(M).getMMI().getMachineFunction(*EntryF);
  assert(EntryMF);
  auto MemorySeed = Cfg.ProgramCfg.MemoryCfg.MemorySeed;
  // this should be assert but asserts are disabled on release
  assert(!EntryMF->empty());
  auto &MBB = EntryMF->front();
  auto Ins = MBB.begin();
  InstructionGenerationContext InstrGenCtx{MBB, Ins, SGCtx};
  if (!MemorySeed)
    snippy::fatal("Missing memory seed value");
  MemManager.createFullRuntimeInit(InstrGenCtx, MemorySeed.value(),
                                   ExternalCallOfMemInitRoutine);
}

template <typename Init>
void processLoadsRuntimeInit(MemoryManager &MemManager, const Config &Cfg,
                             bool ExternalCallOfMemInitRoutine, Module &M,
                             GeneratorContext &SGCtx) {
  auto *EntryF = M.getFunction(Cfg.ProgramCfg.EntryPointName);
  assert(EntryF);
  auto *EntryMF =
      SnippyModule::fromModule(M).getMMI().getMachineFunction(*EntryF);
  assert(EntryMF && !EntryMF->empty());
  auto &MBB = EntryMF->front();
  auto Ins = MBB.begin();
  InstructionGenerationContext InstrGenCtx{MBB, Ins, SGCtx};
  MemManager.createLoadsRuntimeInit<Init>(InstrGenCtx,
                                          ExternalCallOfMemInitRoutine,
                                          Cfg.ProgramCfg.MemoryCfg.MemorySeed);
}

template <typename ParserT>
void processFromFileInit(MemoryManager &MemManager, const Config &Cfg,
                         GeneratorContext &SGCtx) {
  auto &MemCfg = Cfg.ProgramCfg.MemoryCfg;
  assert(MemCfg.MemoryFile);
  MemManager.loadMemStateFromFile<ParserT>(*MemCfg.MemoryFile, SGCtx);
}

void addSectionsToInit(MemoryManager &MemManager, Linker &L) {
  auto &Sections = L.sections();
  auto SCSectionIt =
      std::find_if(Sections.begin(), Sections.end(),
                   [](const Linker::SectionEntry &SectEntry) {
                     return SectEntry.OutputSection.Desc.getName() ==
                            SectionsDescriptions::SelfcheckSectionName;
                   });
  if (SCSectionIt != Sections.end())
    MemManager.addWriteOnlySection(SCSectionIt->OutputSection.Desc);
}

} // namespace

bool MemoryInitializer::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &MemManager = ProgCtx.getMemoryManager();
  auto &Linker = ProgCtx.getLinker();
  auto &Cfg = SGCtx.getConfig();
  auto MemMode = Cfg.ProgramCfg.MemoryCfg.InitializationMode;
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  addSectionsToInit(MemManager, Linker);

  if (MemMode.isDuringRuntime()) {
    auto *EntryF = M.getFunction(Cfg.ProgramCfg.EntryPointName);
    assert(EntryF);
    auto *EntryMF =
        SnippyModule::fromModule(M).getMMI().getMachineFunction(*EntryF);
    auto &SFM = getAnalysis<SnippyFunctionMetadataWrapper>().get(*EntryMF);
    auto *BlockMemInit = createMachineBasicBlock(*EntryMF);
    BlockMemInit->addSuccessor(&EntryMF->front());
    EntryMF->insert(EntryMF->begin(), BlockMemInit);
    SFM.MemInitBlock = BlockMemInit;
  }

  switch (MemMode.Value) {
  case MemInitMode::Full:
    processObjectFileInitFull<MemInitializationPseudoRandom>(
        M, MemManager, Cfg, Linker, SGCtx, SimCtx);
    break;
  case MemInitMode::FullWithAddresses:
    processObjectFileInitFull<MemInitializationWithAddresses>(
        M, MemManager, Cfg, Linker, SGCtx, SimCtx);
    break;
  case MemInitMode::RuntimeFull:
    processFullRuntimeInit(MemManager, Cfg, ExternalCallOfMemInitRoutine, M,
                           SGCtx);
    break;
  case MemInitMode::LoadsOnly:
    processLoadsRuntimeInit<MemInitializationPseudoRandom>(
        MemManager, Cfg, ExternalCallOfMemInitRoutine, M, SGCtx);
    break;
  case MemInitMode::LoadsWithAddresses:
    processLoadsRuntimeInit<MemInitializationWithAddresses>(
        MemManager, Cfg, ExternalCallOfMemInitRoutine, M, SGCtx);
    break;
  case MemInitMode::TextFileInit:
    processFromFileInit<ASCIIDumpParser>(MemManager, Cfg, SGCtx);
    break;
  case MemInitMode::NoInit:
    break;
  }

  // FIXME: Remove random registers from __snippy_random in order to keep traces
  // in
  //  -init-memory=full and -init-memory=runtime modes the same
  RandEngine::reInit();

  auto MemState = MemManager.getMemState();
  if (SimCtx.hasTrackingMode())
    SimCtx.getSimRunner().initInterpretersMemory(MemState.begin(),
                                                 MemState.end());
  return true;
}

} // namespace snippy
} // namespace llvm
