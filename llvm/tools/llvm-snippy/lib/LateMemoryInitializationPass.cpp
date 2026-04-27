//===-- LateMemoryInitializationPass.cpp -------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"

namespace llvm {
namespace snippy {
namespace {

#define DEBUG_TYPE "snippy-memory-postprocess"
#define PASS_DESC "Snippy Memory Postrocess"

struct LateMemoryInitialization final : public ModulePass {
  static char ID;

  LateMemoryInitialization() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<SimulatorContextWrapper>();
    ModulePass::getAnalysisUsage(AU);
  }
};

char LateMemoryInitialization::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LateMemoryInitialization;

INITIALIZE_PASS_BEGIN(LateMemoryInitialization, DEBUG_TYPE, PASS_DESC, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(LateMemoryInitialization, DEBUG_TYPE, PASS_DESC, false,
                    false)

namespace llvm {

ModulePass *createLateMemoryInitializationPass() {
  return new LateMemoryInitialization();
}

namespace snippy {

bool LateMemoryInitialization::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  auto &MemManager = ProgCtx.getMemoryManager();
  auto MemState = MemManager.getMemState();
  auto &Linker = ProgCtx.getLinker();

  if (auto &&SelfcheckCfg = SGCtx.getConfig().getTrackCfg().Selfcheck;
      !SelfcheckCfg || SelfcheckCfg->Mode != SelfcheckMode::Memory)
    return false;

  MemManager.materializeWriteOnlySections(M, SGCtx);
  auto WriteOnlySections =
      make_filter_range(MemState, [](const SectionData &SectData) {
        return SectData.HasLaterInit;
      });
  std::for_each(WriteOnlySections.begin(), WriteOnlySections.end(),
                [&Linker](const SectionData &SectData) {
                  Linker.sections().addInputSectionFor(SectData.Desc,
                                                       SectData.Desc.getName());
                });
  return true;
}

} // namespace snippy
} // namespace llvm
