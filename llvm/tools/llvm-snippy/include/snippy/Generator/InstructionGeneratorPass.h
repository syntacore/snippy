//===-- InstructionGeneratorPass.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "snippy/ActiveImmutablePass.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/Generation.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/MemAccessInfo.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"

namespace llvm {
namespace snippy {

class InstructionGenerator final
    : public ActiveImmutablePass<MachineFunctionPass, MemAccessInfo> {
  planning::FunctionRequest
  createMFGenerationRequest(const MachineFunction &MF) const;

  void finalizeFunction(MachineFunction &MF, planning::FunctionRequest &Request,
                        const GenerationStatistics &MFStats);

  void prepareInterpreterEnv() const;

  void addGV(const APInt &Value, unsigned long long Stride,
             GlobalValue::LinkageTypes LType, StringRef Name) const;

  void addSelfcheckSectionPropertiesAsGV() const;

  void addModelMemoryPropertiesAsGV() const;

  GeneratorContext *SGCtx;

public:
  static char ID;

  InstructionGenerator()
      : ActiveImmutablePass<MachineFunctionPass, MemAccessInfo>(ID) {}

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace snippy
} // namespace llvm
