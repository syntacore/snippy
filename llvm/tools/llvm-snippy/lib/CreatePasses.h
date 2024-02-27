//===-- CreatePasses.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "FlowGenerator.h"

#include "llvm/ADT/StringRef.h"

namespace llvm {

class ImmutablePass;
class ModulePass;
class MachineFunctionPass;
class MachineModuleInfo;
class raw_ostream;

namespace snippy {

class LLVMState;
class MemoryScheme;
class OpcodeCache;
class GeneratorContext;
struct GeneratorSettings;
class MemoryManager;

} // namespace snippy

ImmutablePass *createGeneratorContextWrapperPass(snippy::GeneratorContext &Ctx);

ImmutablePass *createRootRegPoolWrapperPass();

ModulePass *createFunctionDistributePass();

ModulePass *createFunctionGeneratorPass();

ModulePass *createReserveRegsPass();

ModulePass *createFillExternalFunctionsStubsPass(
    const std::vector<std::string> &FunctionsToAvoid);

MachineFunctionPass *createCFGeneratorPass();

MachineFunctionPass *createCFPermutationPass();

MachineFunctionPass *createLoopCanonicalizationPass();

MachineFunctionPass *createLoopLatcherPass();

MachineFunctionPass *createCFGPrinterPass();

MachineFunctionPass *createInstructionGeneratorPass();

MachineFunctionPass *createLoopAlignmentPass();

MachineFunctionPass *createInstructionsPostProcessPass();

MachineFunctionPass *createBranchRelaxatorPass();

MachineFunctionPass *createRegsInitInsertionPass(bool InitRegs);

MachineFunctionPass *createPrologueEpilogueInsertionPass();

MachineFunctionPass *createPrintMachineInstrsPass(raw_ostream &OS);

MachineFunctionPass *createBlockGenPlanningPass();

} // namespace llvm
