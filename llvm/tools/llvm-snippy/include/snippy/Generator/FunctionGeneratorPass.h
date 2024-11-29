//===-- FunctionGeneratorPass.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Generator/CallGraphState.h"
#include "snippy/Generator/Linker.h"

#include "snippy/ActiveImmutablePass.h"

namespace llvm {
namespace snippy {

class GeneratorContext;

struct GlobalCodeFlowInfo {
  CallGraphState CGS;
  std::vector<Linker::SectionEntry> ExecutionPath;
};

class FunctionGenerator final
    : public ActiveImmutablePass<ModulePass, GlobalCodeFlowInfo> {
public:
  static char ID;

  FunctionGenerator()
      : ActiveImmutablePass<ModulePass, GlobalCodeFlowInfo>(ID){};

  StringRef getPassName() const override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnModule(Module &M) override;

  const auto &getCallGraphState() const {
    return get<GlobalCodeFlowInfo>().CGS;
  }

  auto isEntryFunction(const MachineFunction &MF) const {
    return getCallGraphState().isEntryFunction(MF);
  }

  auto isExitFunction(const MachineFunction &MF) const {
    return getCallGraphState().isExitFunction(MF);
  }

  auto isRootFunction(const Function &F) const {
    return getCallGraphState().isRoot(&F);
  }
  auto isRootFunction(const MachineFunction &MF) const {
    return isRootFunction(MF.getFunction());
  }

  auto *nextRootFunction(const MachineFunction &MF) const {
    return getCallGraphState().nextRootFunction(MF);
  }

  size_t getRequestedInstrsNum(const MachineFunction &MF) const {
    assert(RequestedInstrNum.count(&MF));
    return RequestedInstrNum.at(&MF);
  }

  void setRequestedInstrNum(const MachineFunction &MF, size_t NumInstr) {
    RequestedInstrNum.emplace(&MF, NumInstr);
  }

private:
  bool readFromYaml(Module &M, const FunctionDescs &FDs);

  bool generateDefault(Module &M);

  MachineFunction &createFunction(GeneratorContext &SGCtx, Module &M,
                                  StringRef Name, StringRef SectionName,
                                  Function::LinkageTypes Linkage,
                                  size_t NumInstr);

  std::vector<std::string> prepareRXSections();
  struct RootFnPlacement {
    std::string SectionName;
    size_t InstrNum;
    RootFnPlacement(StringRef Name, size_t IN)
        : SectionName{Name}, InstrNum{IN} {};
  };
  std::vector<RootFnPlacement> distributeRootFunctions();

  void initRootFunctions(Module &M, StringRef EntryPointName);

  void initExecutionPath();

  std::unordered_map<const MachineFunction *, size_t> RequestedInstrNum;
};

} // namespace snippy
} // namespace llvm
