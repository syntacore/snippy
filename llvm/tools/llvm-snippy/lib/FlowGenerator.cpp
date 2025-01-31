//===-- FlowGenerator.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FlowGenerator.h"
#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/IntervalsToVerify.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/InitializePasses.h"
#include "snippy/PassManagerWrapper.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

namespace llvm {

namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string>
    RegGeneratorFile("reg-generator-plugin",
                     cl::desc("Plugin for custom registers generation."
                              "Use =None to generate registers "
                              "with build-in randomizer."
                              "(=None - default value)"),
                     cl::value_desc("filename"), cl::cat(Options),
                     cl::init("None"));

static snippy::opt<bool> SelfCheckMem(
    "selfcheck-mem",
    cl::desc("check a memory state after execution in selfcheck mode"),
    cl::Hidden, cl::init(true));

static snippy::opt_list<std::string>
    DumpMemorySection("dump-memory-section", cl::CommaSeparated,
                      cl::desc("Dump listed memory sections"
                               "after interpretation. "
                               "If {rw} specified, "
                               "then all read and write sections "
                               "will be dumped. "
                               "(similarly for other accesses)"),
                      cl::cat(Options));

static snippy::opt<std::string>
    MemorySectionFile("memory-section-file",
                      cl::desc("file to dump specified section"),
                      cl::cat(Options), cl::init("mem_state.bin"));

static snippy::opt<std::string>
    RegInfoFile("reg-plugin-info-file",
                cl::desc("File with info for registers generator. "
                         "Use =None if plugin doesn't need additional info."
                         "(=None - default value)"),
                cl::value_desc("filename"), cl::cat(Options), cl::init("None"));

static snippy::opt<std::string>
    DumpMIR("dump-mir", cl::ValueOptional,
            cl::desc("Request dump the Machine IR."),
            cl::value_desc("filename"), cl::Hidden, cl::init("a.mir"),
            cl::cat(Options));

snippy::opt<bool> VerifyConsecutiveLoops(
    "verify-consecutive-loops",
    cl::desc(
        "Check that consecutive loops generated accordingly to branchegram."),
    cl::cat(Options), cl::init(false), cl::Hidden);

} // namespace snippy

namespace snippy {

static snippy::opt<std::string>
    RegionsToVerify("dump-intervals-to-verify",
                    cl::desc("Save PC intervals that can be verified (YAML)"),
                    cl::cat(Options), cl::ValueOptional, cl::init(""));

namespace {

void writeMIRFile(StringRef Data) {
  auto Path = DumpMIR.getValue();
  if (Path.empty())
    Path = DumpMIR.getDefault().getValue();
  writeFile(Path, Data);
}

} // namespace

static void dumpVerificationIntervalsIfNeeeded(SnippyModule &SM,
                                               const GeneratorContext &GenCtx) {
  if (!RegionsToVerify.isSpecified())
    return;

  StringRef Output = SM.getGeneratedObject();
  auto &ProgCtx = GenCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &Ctx = State.getCtx();
  ObjectMetadata NullObjMeta{};
  auto &Meta = SM.hasGenResult<ObjectMetadata>()
                   ? SM.getGenResult<ObjectMetadata>()
                   : NullObjMeta;
  auto VerificationIntervals = IntervalsToVerify::createFromObject(
      State.getDisassembler(), Output,
      GenCtx.getProgramContext().getEntryPointName(),
      ProgCtx.getLinker().sections().getOutputSectionFor(".text").Desc.VMA,
      Meta.EntryPrologueInstrCnt, Meta.EntryEpilogueInstrCnt);

  if (!VerificationIntervals)
    snippy::fatal(Ctx, "Failed to extract pc intervals to verify",
                  VerificationIntervals.takeError());

  const auto &GenSettings = GenCtx.getGenSettings();
  auto RegionsToVerifyFilename =
      RegionsToVerify.isSpecified() && RegionsToVerify.getValue().empty()
          ? addExtensionIfRequired(GenSettings.BaseFileName,
                                   ".intervals-to-verify.yaml")
          : RegionsToVerify.getValue();

  if (auto E = VerificationIntervals->dumpAsYaml(RegionsToVerifyFilename))
    snippy::fatal(Ctx, "Failed to save YAML to " + RegionsToVerifyFilename,
                  std::move(E));
}

static RegisterGenerator createRegGen(std::string PluginFileName,
                                      std::string InfoFileName) {
  if (PluginFileName == "None")
    PluginFileName = "";
  if (InfoFileName == "None")
    InfoFileName = "";
  return RegisterGenerator{PluginFileName, InfoFileName};
}

GeneratorResult FlowGenerator::generate(LLVMState &State) {

  auto RegGen =
      createRegGen(RegGeneratorFile.getValue(), RegInfoFile.getValue());

  SnippyProgramContext ProgContext(State, RegGen, RP, OpCC,
                                   GenSettings.getSnippyProgramSettings(State));

  const auto &SnippyTgt = State.getSnippyTarget();
  auto MainModule = SnippyModule(ProgContext.getLLVMState(), "main");

  GeneratorContext GenCtx(ProgContext, GenSettings);

  std::string MIR;
  raw_string_ostream MIROS(MIR);

  MainModule.generateObject(
      [&](PassManagerWrapper &PM) {
        // Pre backtrack start
        PM.add(createGeneratorContextWrapperPass(GenCtx));
        PM.add(createRootRegPoolWrapperPass());
        PM.add(createFunctionGeneratorPass());
        PM.add(createSimulatorContextWrapperPass(/* DoInit */ true));

        PM.add(createReserveRegsPass());
        PM.add(createCFGeneratorPass());
        PM.add(createCFPermutationPass());

        PM.add(createLoopAlignmentPass());
        PM.add(createLoopCanonicalizationPass());
        PM.add(createLoopLatcherPass());

        PM.add(createRegsInitInsertionPass(
            GenSettings.RegistersConfig.InitializeRegs));
        SnippyTgt.addTargetSpecificPasses(PM);
        // Pre backtrack end

        PM.add(createBlockGenPlanWrapperPass());
        PM.add(createBlockGenPlanningPass());
        PM.add(createInstructionGeneratorPass()); // Can be backtracked

        // Post backtrack
        PM.add(createPrologueEpilogueInsertionPass());
        PM.add(createFillExternalFunctionsStubsPass({}));
        if (GenSettings.DebugConfig.PrintMachineFunctions)
          PM.add(createMachineFunctionPrinterPass(outs()));

        if (GenSettings.DebugConfig.PrintInstrs)
          PM.add(createPrintMachineInstrsPass(outs()));

        SnippyTgt.addTargetLegalizationPasses(PM);
        PM.add(createBranchRelaxatorPass());
        if (VerifyConsecutiveLoops)
          PM.add(createConsecutiveLoopsVerifierPass());

        PM.add(createPostGenVerifierPass());
        PM.add(createInstructionsPostProcessPass());
        PM.add(createFunctionDistributePass());

        if (GenSettings.InstrsGenerationConfig.RunMachineInstrVerifier)
          PM.add(createMachineVerifierPass("Machine Verifier Pass report"));

        if (GenSettings.DebugConfig.PrintControlFlowGraph)
          PM.add(createCFGPrinterPass(
              GenSettings.DebugConfig.ViewControlFlowGraph));

        if (DumpMIR.isSpecified())
          PM.add(createPrintMIRPass(MIROS));
        PM.add(createMemAccessDumperPass());
      },
      [](PassManagerWrapper &PM) {
        PM.add(createSimulatorContextPreserverPass());
      });

  if (DumpMIR.isSpecified())
    writeMIRFile(MIR);
  std::vector<const SnippyModule *> Modules{&MainModule};
  auto Result = ProgContext.generateELF(Modules);

  dumpVerificationIntervalsIfNeeeded(MainModule, GenCtx);

  if (GenSettings.ModelPluginConfig.RunOnModel) {
    auto SnippetImageForModelExecution =
        ProgContext.generateLinkedImage(Modules);

    auto RI = SimulatorContext::RunInfo{
        SnippetImageForModelExecution, ProgContext, MainModule,
        GenSettings.LinkerConfig.EntryPointName,
        GenSettings.RegistersConfig.InitialStateOutputYaml,
        GenSettings.RegistersConfig.FinalStateOutputYaml, SelfCheckMem,
        // Memory reset only needed if interpreter may have executed
        // during generation process.
        /* NeedMemoryReset */ GenSettings.hasTrackingMode(), DumpMemorySection,
        MemorySectionFile.getValue(), GenSettings.BaseFileName};

    auto &SimCtx = MainModule.getGenResult<OwningSimulatorContext>();
    SimCtx.runSimulator(RI);

  } else {

    snippy::warn(WarningName::NoModelExec, State.getCtx(),
                 "Skipping snippet execution on the model",
                 "model was set no 'None'.");
  }
  if (!Result.LinkerScript.empty())
    snippy::notice(
        WarningName::RelocatableGenerated, State.getCtx(),
        "Snippet generator generated relocatable image",
        "please, use linker with provided script to generate final image");

  return Result;
}

} // namespace snippy
} // namespace llvm
