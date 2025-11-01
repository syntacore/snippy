//===-- GeneratorContext.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/RandomMemAccSampler.h"
#include "snippy/Simulator/SelfcheckObserver.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"

#include <set>
#include <vector>

#define DEBUG_TYPE "snippy-gen-context"

namespace llvm {
namespace snippy {

template class GenResultT<SelfcheckMap>;

extern cl::OptionCategory Options;

static snippy::opt<bool> ForceEmitLinkerScript(
    "force-emit-linker-script",
    cl::desc("Snippy will emit linker script even if linker not being run "),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    DumpRandMemAccesses("dump-rand-mem-accesses",
                        cl::desc("Dump random memory accesses"),
                        cl::cat(Options), cl::Hidden);

GeneratorContext::GeneratorContext(SnippyProgramContext &ProgCtx,
                                   Config &Settings)
    : ProgContext(&ProgCtx), Cfg(&Settings), MemAccSampler([&] {
        std::vector<SectionDesc> RWSections;
        llvm::copy_if(Settings.ProgramCfg.Sections,
                      std::back_inserter(RWSections),
                      [](auto &Desc) { return Desc.M.W() && Desc.M.R(); });
        auto &MS = Settings.CommonPolicyCfg->MS;
        auto &TM = ProgContext->getLLVMState().getTargetMachine();
        auto Alignment =
            TM.createDataLayout().getPointerABIAlignment(/* Address Space */ 0);
        auto BaseAccesses = llvm::map_range(
            MS.BaseAccesses,
            [](const auto &A) -> const MemoryAccess & { return *A; });
        auto RandSampler = std::make_unique<RandomMemoryAccessSampler>(
            RWSections.begin(), RWSections.end(), BaseAccesses.begin(),
            BaseAccesses.end(), Alignment.value(), MS.Restricted);
        if (DumpRandMemAccesses)
          RandSampler->dump();
        std::vector<std::unique_ptr<IMemoryAccessSampler>> Samplers;
        Samplers.emplace_back(std::move(RandSampler));
        return TopLevelMemoryAccessSampler(Samplers.begin(), Samplers.end());
      }()) {
  // HACK: Here we create dummy module and function to create a
  // TargetSubtargetInfo. These are destroyed right after createTargetContext
  // call
  auto &State = ProgContext->getLLVMState();
  auto DummyModule = Module("__snippy_dummy_module", State.getCtx());
  auto &Dummy = State.createFunction(DummyModule, "__dummy", "",
                                     Function::LinkageTypes::InternalLinkage);
  ProgContext->createTargetContext(Settings, State.getSubtargetImpl(Dummy));
  Dummy.eraseFromParent();
}

} // namespace snippy
} // namespace llvm
