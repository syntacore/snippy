//===-- InitializePasses.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "InitializePasses.h"

#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/RootRegPoolWrapperPass.h"
#include "snippy/Generator/SMCInitPass.h"
#include "snippy/InitializePasses.h"

void llvm::snippy::initializeSnippyPasses(PassRegistry &Registry) {
  initializeSnippyFunctionMetadataWrapperPass(Registry);
  initializeGeneratorContextWrapperPass(Registry);
  initializeRootRegPoolWrapperPass(Registry);
  initializeReserveRegsPass(Registry);
  initializeFillExternalFunctionsStubsPass(Registry);
  initializeMemoryInitializerPass(Registry);
  initializeLateMemoryInitializationPass(Registry);
  initializeFunctionGeneratorPass(Registry);
  initializeSimulatorContextWrapperPass(Registry);
  initializeSimulatorContextPreserverPass(Registry);
  initializeFunctionDistributePass(Registry);
  initializeCFGeneratorPass(Registry);
  initializeCFPermutationPass(Registry);
  initializeLoopCanonicalizationPass(Registry);
  initializeLoopLatcherPass(Registry);
  initializeLoopAlignmentPass(Registry);
  initializeCFGPrinterPass(Registry);
  initializeInstructionGeneratorPass(Registry);
  initializeRegsInitInsertionPass(Registry);
  initializePrologueEpilogueInsertionPass(Registry);
  initializePrintMachineInstrsPass(Registry);
  initializeTrackLivenessPass(Registry);
  initializeInstructionsPostProcessPass(Registry);
  initializeBranchRelaxatorPass(Registry);
  initializeBlockGenPlanningPass(Registry);
  initializeMemoryAccessDumperPass(Registry);
  initializeMemInitGeneratorPass(Registry);
  initializeSMCFillerPass(Registry);
  initializeSMCGeneratorPass(Registry);
  initializeSMCInitPass(Registry);
  initializeSMCSetSizeGlobalsPass(Registry);
  initializeSMCBBToNamePass(Registry);
  initializeConsecutiveLoopsVerifierPass(Registry);
  initializeCLBasicBlockPreprocessPass(Registry);
  initializeConfigureLinkerPass(Registry);
  initializeJumpLengthenerPass(Registry);
}
