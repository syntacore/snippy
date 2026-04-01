//===-- ConfigureLinkerPass.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContextPass.h"

#define DEBUG_TYPE "snippy-configure-linker"
#define PASS_DESC "Snippy Configure Linker"

namespace llvm {
namespace snippy {
namespace {

class ConfigureLinker final : public MachineFunctionPass {
public:
  static char ID;

  ConfigureLinker() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<FunctionGenerator>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char ConfigureLinker::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::ConfigureLinker;

INITIALIZE_PASS(ConfigureLinker, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createLinkerConfigurePass() {
  return new snippy::ConfigureLinker;
}

namespace snippy {

bool ConfigureLinker::runOnMachineFunction(MachineFunction &MF) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  if (!SGCtx.getConfig().PassCfg.CodeLayout)
    return false;
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &Linker = ProgCtx.getLinker();
  const auto &FG = getAnalysis<FunctionGenerator>();
  if (!FG.isEntryFunction(MF))
    return false;

  // Setup Start PC:
  assert(MF.getSection());
  Linker.setStartPC(
      Linker.sections().getAddressFor(MF.getFunction().getSection()));

  return false;
}

} // namespace snippy
} // namespace llvm
