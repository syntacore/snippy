//===-- InitializePasses.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {

class PassRegistry;

void initializeGeneratorContextWrapperPass(PassRegistry &);
void initializeRootRegPoolWrapperPass(PassRegistry &);
void initializeReserveRegsPass(PassRegistry &);
void initializeFillExternalFunctionsStubsPass(PassRegistry &);
void initializeRegisterStateWrapperPass(PassRegistry &);
void initializeFunctionGeneratorPass(PassRegistry &);
void initializeFunctionDistributePass(PassRegistry &);
void initializeCFGeneratorPass(PassRegistry &);
void initializeCFPermutationPass(llvm::PassRegistry &);
void initializeLoopCanonicalizationPass(llvm::PassRegistry &);
void initializeLoopLatcherPass(llvm::PassRegistry &);
void initializeLoopAlignmentPass(llvm::PassRegistry &);
void initializeCFGPrinterPass(PassRegistry &);
void initializeInstructionGeneratorPass(PassRegistry &);
void initializeRegsInitInsertionPass(PassRegistry &);
void initializePrologueEpilogueInsertionPass(PassRegistry &);
void initializePrintMachineInstrsPass(PassRegistry &);
void initializeInstructionsPostProcessPass(PassRegistry &);
void initializeBranchRelaxatorPass(PassRegistry &);
void initializeBlockGenPlanningPass(PassRegistry &);
void initializeConsecutiveLoopsVerifierPass(PassRegistry &);

} // namespace llvm
