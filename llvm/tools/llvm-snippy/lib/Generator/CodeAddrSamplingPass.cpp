//===-- CodeAddrSamplingPass.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../InitializePasses.h"

#include "snippy/Generator/CodeAddrSampler.h"
#include "snippy/Generator/CodeAddrSamplingPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/SMCManager.h"

#define DEBUG_TYPE "snippy-code-addr-sampler"
#define PASS_DESC "Snippy Code Address Sampling"

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::CodeAddrSampling;

char CodeAddrSampling::ID = 0;

INITIALIZE_PASS_BEGIN(CodeAddrSampling, DEBUG_TYPE, PASS_DESC, false, true)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(CodeAddrSampling, DEBUG_TYPE, PASS_DESC, false, true)

namespace llvm {

MachineFunctionPass *createCodeAddrSamplingPass() {
  return new snippy::CodeAddrSampling;
}

namespace snippy {

void CodeAddrSampling::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<GeneratorContextWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool CodeAddrSampling::runOnMachineFunction(MachineFunction &MF) {
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &Cfg = GC.getConfig().PassCfg;
  assert(Cfg.CodeLayout);
  auto &ProgCtx = GC.getProgramContext();
  auto &SM = SnippyModule::fromModule(*MF.getFunction().getParent());
  const auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto &Sampler = ProgCtx.getOrCreateAddrSampler(Cfg);
  auto &Linker = ProgCtx.getLinker();
  auto SampleAddrForBB = [&](auto &MBB, StringRef GVName,
                             StringRef InputSectionName) {
    auto &State = ProgCtx.getLLVMState();
    auto BlockSize = State.getCodeBlockSize(MBB.begin(), MBB.end());
    auto &STI = MBB.getParent()->getSubtarget();
    AddressGenInfo Params{BlockSize, Tgt.getCodeAlignment(STI),
                          /* AllowMisalign */ false,
                          /* Burst */ false};
    auto AI = Sampler.randomAddress(Params);
    Linker.sections().addInputSection(InputSectionName, AI);
    auto &TM = State.getTargetMachine();
    auto AddrLen = Tgt.getAddrRegLen(TM);
    auto &InstrGenCfg = Cfg.InstrsGenerationConfig;
    if (!InstrGenCfg.NeedsRelocations && !Cfg.SMC.has_value())
      return;
    auto &GP = ProgCtx.getOrAddGlobalsPoolFor(
        SM, "Failed to allocate space for relocation for BB "
            "address (code addr sampler)");
    auto *GV = GP.getGV(GVName);
    if (!GV)
      GV = GP.createGV(
          APInt(AddrLen, 3), /*Alignment*/ 1, GlobalValue::InternalLinkage,
          GVName, /*Reason*/ "Relocation for BB address", /* IsConst */ true);
    auto *Type = IntegerType::get(State.getCtx(), AddrLen);
    auto *AddrConstant = ConstantInt::get(Type, AI.Address);
    GV->setInitializer(AddrConstant);
  };

  StringRef FirstBlockName = MF.getFunction().getSection();
  SampleAddrForBB(MF.front(), FirstBlockName, FirstBlockName);
  auto &SMCManager = ProgCtx.getSMCManager();
  if (MF.getName() == SMCManagerT::SMCSrcFuncName) {
    for (auto &&[MBB, Name] :
         zip(drop_begin(MF), SMCManager.getSrcNamesFromSMCBlockPairs()))
      SampleAddrForBB(MBB, Name, getMBBSectionName(MBB));
    return false;
  }
  for (auto &MBB : llvm::drop_begin(MF))
    SampleAddrForBB(MBB, getMBBSectionName(MBB), getMBBSectionName(MBB));
  return false;
}

} // namespace snippy
} // namespace llvm
