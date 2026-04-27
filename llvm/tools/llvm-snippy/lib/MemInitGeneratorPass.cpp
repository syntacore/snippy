//===-- MemInitGeneratorPass.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "InitializePasses.h"

#include "snippy/Config/MemInitializationMode.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/Policy.h"

#include "llvm/CodeGen/MachineModuleInfo.h"

#define DEBUG_TYPE "snippy-memory-initializer"
#define PASS_DESC "Snippy Memory Initializer Generator"

namespace llvm {
namespace snippy {
namespace {

class MemInitGenerator final : public ModulePass {
  MemoryManager *MemManager = nullptr;
  const LLVMState *State = nullptr;
  MachineModuleInfo *MMI = nullptr;
  MemInitMode InitMode = MemInitMode::NoInit;
  void generateLoadsInit(Module &M);
  void generateRandomGen(Module &M);

public:
  static char ID;

  MemInitGenerator() : ModulePass(ID) {}
  MemInitGenerator(MemoryManager &MemManager, const LLVMState &State,
                   MachineModuleInfo &MMI, MemInitMode InitMode)
      : ModulePass(ID), MemManager{&MemManager}, State{&State}, MMI{&MMI},
        InitMode{InitMode} {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<GeneratorContextWrapper>();
  }
};

char MemInitGenerator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::MemInitGenerator;

INITIALIZE_PASS(MemInitGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createMemInitGeneratorPass(snippy::MemoryManager &MemManager,
                                       const snippy::LLVMState &State,
                                       MachineModuleInfo &MMI,
                                       snippy::MemInitMode InitMode) {
  return new MemInitGenerator(MemManager, State, MMI, InitMode);
}

namespace snippy {

template <typename It>
static std::map<MemAddr, MemoryUnit> accumulateAddresses(It SectDataBeg,
                                                         It SectDataEnd) {
  auto GlobalAddresses = MemoryMap{};

  for (const auto &Sect : make_range(SectDataBeg, SectDataEnd)) {
    auto OrderedAddrToInit = std::set<MemAddr>{Sect.AddressesToInit.begin(),
                                               Sect.AddressesToInit.end()};
    std::transform(OrderedAddrToInit.begin(), OrderedAddrToInit.end(),
                   std::inserter(GlobalAddresses, GlobalAddresses.end()),
                   [&Sect](MemAddr LocalAddrToInit) {
                     auto GlobalAddr = LocalAddrToInit + Sect.Desc.VMA;
                     const auto &MemMap = Sect.MemMap;
                     assert(Sect.isAccessValid(GlobalAddr, 1u) &&
                            "Invalid address");
                     auto MemValIt = MemMap.find(LocalAddrToInit);
                     assert(MemValIt != MemMap.end());
                     return std::make_pair(GlobalAddr, MemValIt->second);
                   });
  }
  return GlobalAddresses;
}
void MemInitGenerator::generateRandomGen(Module &M) {
  auto &SnpTgt = State->getSnippyTarget();
  auto RandFuncName = MemManager->getExternalRandomizerName();
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &MF = State->createMachineFunctionFor(
      State->createFunction(M, RandFuncName, "", Function::ExternalLinkage,
                            M.getContext()),
      *MMI, M.getContext(), GC.getConfig().PassCfg.CodeLayout.has_value());
  auto *MBB = createMachineBasicBlock(MF);
  assert(MBB);
  MF.push_back(MBB);

  InstructionGenerationContext IGC{*MBB, MBB->end(), GC};

  SnpTgt.generateRandomGenFunction(IGC);

  SnpTgt.generateReturn(IGC, IGC.ProgCtx.getReturnAddress());

  assert(GC.getProgramContext().getLLVMState().getFunctionSize(MF) <=
             SnpTgt.getRandomGenFunctionMaxSize() &&
         "Actual size should not exceed hardcoded estimate size");
}

void MemInitGenerator::generateLoadsInit(Module &M) {
  auto &SnpTgt = State->getSnippyTarget();
  auto RandFuncName = MemManager->getExternalRandomizerName();
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();

  auto MemState = MemManager->getMemState();
  auto AddressesToInit = accumulateAddresses(MemState.begin(), MemState.end());

  auto &MF =
      State->createMachineFunction(M, *MMI, RandFuncName, /* SectionName */ "",
                                   Function::ExternalLinkage, M.getContext());
  auto *MBB = createMachineBasicBlock(MF);
  assert(MBB);
  MF.push_back(MBB);
  InstructionGenerationContext IGC{*MBB, MBB->getFirstTerminator(), GC};

  SnpTgt.generateMemorytInitializationAtAddresses(IGC, AddressesToInit);
  SnpTgt.generateReturn(IGC, IGC.ProgCtx.getReturnAddress());
}

bool MemInitGenerator::runOnModule(Module &M) {
  assert(InitMode.isDuringRuntime());
  auto RandFuncName = MemManager->getExternalRandomizerName();
  if (InitMode == MemInitMode::RuntimeFull) {
    generateRandomGen(M);
    return true;
  }
  if (InitMode.isLoadsInit()) {
    generateLoadsInit(M);
    return true;
  }
  return false;
}
} // namespace snippy
} // namespace llvm
