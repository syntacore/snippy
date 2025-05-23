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
class MachineLoopInfo;
namespace snippy {

namespace planning {
class InstructionGroupRequest;
class BasicBlockRequest;
class FunctionRequest;
class InstructionGenerationContext;
} // namespace planning

class GeneratorContext;
struct GenerationStatistics;
struct SelfCheckInfo;
struct SimulatorContext;
class CallGraphState;
class MemAccessInfo;
class SnippyLoopInfo;
struct SnippyFunctionMetadata;
class DefaultPolicyConfig;

void generate(planning::InstructionGroupRequest &IG,
              planning::InstructionGenerationContext &InstrGenCtx,
              const DefaultPolicyConfig *FBC);

GenerationStatistics
generate(planning::BasicBlockRequest &BB,
         planning::InstructionGenerationContext &InstrGenCtx,
         const DefaultPolicyConfig *FBC);

void generate(planning::FunctionRequest &FunctionGenRequest,
              MachineFunction &MF, GeneratorContext &GC,
              const SimulatorContext &SimCtx, MachineLoopInfo *MLI = nullptr,
              const CallGraphState *CGS = nullptr, MemAccessInfo *MAI = nullptr,
              const SnippyLoopInfo *SLI = nullptr,
              SnippyFunctionMetadata *SFM = nullptr);

} // namespace snippy
} // namespace llvm
#endif
