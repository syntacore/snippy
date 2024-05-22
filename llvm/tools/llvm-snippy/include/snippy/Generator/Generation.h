//===-- Generation.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GENERATION_H
#define LLVM_TOOLS_SNIPPY_GENERATION_H

namespace llvm {
class MachineFunction;
namespace snippy {

namespace planning {
class InstructionGroupRequest;
class BasicBlockRequest;
class FunctionRequest;
struct InstructionGenerationContext;
} // namespace planning
class GeneratorContext;
struct GenerationStatistics;
struct SelfCheckInfo;

void generate(planning::InstructionGroupRequest &IG,
              GenerationStatistics &CurrentStats,
              planning::InstructionGenerationContext &InstrGenCtx);

GenerationStatistics
generate(planning::BasicBlockRequest &BB,
         planning::InstructionGenerationContext &InstrGenCtx);

void generate(planning::FunctionRequest &FunctionGenRequest,
              MachineFunction &MF, GeneratorContext &GC,
              SelfCheckInfo *SelfCheckInfo);

} // namespace snippy
} // namespace llvm
#endif
