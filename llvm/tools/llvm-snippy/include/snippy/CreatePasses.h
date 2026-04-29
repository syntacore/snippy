//===-- CreatePasses.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

namespace llvm {

class ImmutablePass;
class ModulePass;
class MachineFunctionPass;
class MachineModuleInfo;
class PassInfo;
class raw_ostream;
class Twine;

namespace snippy {

class ActiveImmutablePassInterface;
class LLVMState;
class MemoryScheme;
class OpcodeCache;
class GeneratorContext;
class MemoryManager;
struct MemInitMode;

} // namespace snippy

ImmutablePass *createSnippyFunctionMetadataWrapperPassWrapperPass();
ImmutablePass *createGeneratorContextWrapperPass(snippy::GeneratorContext &Ctx);

ImmutablePass *createRootRegPoolWrapperPass();

ModulePass *createFunctionDistributePass();

snippy::ActiveImmutablePassInterface *createFunctionGeneratorPass();
snippy::ActiveImmutablePassInterface *
createSimulatorContextWrapperPass(bool DoInit);
ModulePass *createSimulatorContextPreserverPass();

ModulePass *createReserveRegsPass();

ModulePass *createFillExternalFunctionsStubsPass(
    const std::vector<std::string> &FunctionsToAvoid);

MachineFunctionPass *createTrackLivenessPass();

ModulePass *createMemoryInitializerPass(bool ExternalCallOfMemInitRoutine);

ModulePass *createLateMemoryInitializationPass();

MachineFunctionPass *createPreserveRegsInsertionPass();

MachineFunctionPass *createCFGeneratorPass();

snippy::ActiveImmutablePassInterface *createCFPermutationPass();

MachineFunctionPass *createLoopCanonicalizationPass();

snippy::ActiveImmutablePassInterface *createLoopLatcherPass();

MachineFunctionPass *createCFGPrinterPass(bool EnableView);
MachineFunctionPass *createCFGPrinterPassBefore(const PassInfo &PI,
                                                bool EnableView);
MachineFunctionPass *createCFGPrinterPassAfter(const PassInfo &PI,
                                               bool EnableView);

snippy::ActiveImmutablePassInterface *createInstructionGeneratorPass();

MachineFunctionPass *createLoopAlignmentPass();


MachineFunctionPass *createInstructionsPostProcessPass();

MachineFunctionPass *createPostGenVerifierPass();

MachineFunctionPass *createBranchRelaxatorPass();

MachineFunctionPass *createRegsInitInsertionPass(bool InitRegs);

MachineFunctionPass *createPrologueEpilogueInsertionPass();

MachineFunctionPass *createPrintMachineInstrsPass(raw_ostream &OS);

ModulePass *createMemInitGeneratorPass(snippy::MemoryManager &MemManager,
                                       const snippy::LLVMState &State,
                                       MachineModuleInfo &MMI,
                                       snippy::MemInitMode InitMode);

ModulePass *createSMCInitPass(MachineModuleInfo &MMI);
ModulePass *createSMCGeneratorPass(MachineModuleInfo &MMI);
ModulePass *createSMCFillerPass(snippy::LLVMState &State);
ModulePass *createSMCSetSizeGlobalsPass();

ModulePass *createSMCBBToNamePass();

MachineFunctionPass *createBlockGenPlanningPass();
ImmutablePass *createBlockGenPlanWrapperPass();

MachineFunctionPass *createMemAccessDumperPass();

MachineFunctionPass *createConsecutiveLoopsVerifierPass();

MachineFunctionPass *createRISCVExpandSnippyPseudoPass();

MachineFunctionPass *createRISCVZcmpPopretCombinePass();

MachineFunctionPass *createCLBasicBlockPreprocessPass();

MachineFunctionPass *createCodeAddrSamplingPass();

MachineFunctionPass *createFallThroughEraserPass();

MachineFunctionPass *createBranchLengthenerPass();

snippy::ActiveImmutablePassInterface *createJumpLengthenerPass();

MachineFunctionPass *createLongJumpRelaxatorPass();

MachineFunctionPass *createLinkerConfigurePass();
} // namespace llvm
