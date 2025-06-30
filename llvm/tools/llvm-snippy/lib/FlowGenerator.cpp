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
#include "snippy/Generator/SelfCheckInfo.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/InitializePasses.h"
#include "snippy/PassManagerWrapper.h"
#include "snippy/Simulator/SelfcheckObserver.h"
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
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

#define DEBUG_TYPE "snippy-flow-generation"

namespace llvm {

struct ObjectTypeEnumOption
    : public snippy::EnumOptionMixin<ObjectTypeEnumOption> {
  static void doMapping(snippy::EnumMapper &Mapper) {
    Mapper.enumCase(snippy::GeneratorResult::Type::RELOC, "reloc",
                    "generate relocatable elf + linker script");
    Mapper.enumCase(snippy::GeneratorResult::Type::EXEC, "exec",
                    "generate executable elf");
    Mapper.enumCase(snippy::GeneratorResult::Type::DYN, "shared",
                    "generate shared object");
  }
};

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::GeneratorResult::Type,
                                           ObjectTypeEnumOption)

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

static snippy::opt<snippy::GeneratorResult::Type>
    ObjectType("object-type", cl::desc("type of generated object"),
               ObjectTypeEnumOption::getClValues(),
               cl::init(snippy::GeneratorResult::Type::RELOC),
               cl::cat(Options));

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

static snippy::opt<bool> DisableLinkerRelaxations(
    "disable-linker-relaxations",
    cl::desc("Disable linker relaxations for the final image"),
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

static void dumpSelfCheck(const std::vector<char> &Data, size_t ChunkSize,
                          size_t ChunksNum, raw_ostream &OS) {
  for (size_t Offset = 0; Offset < Data.size();
       Offset += ChunkSize * ChunksNum) {
    for (size_t Idx = 0; Idx < ChunkSize * ChunksNum; Idx++) {
      if (Idx % ChunkSize == 0)
        OS << "\n";
      OS.write_hex(static_cast<unsigned char>(Data[Offset + Idx]));
      OS << " ";
    }
    OS << "\n------\n";
  }
}

static void checkMemStateAfterSelfcheck(SnippyProgramContext &ProgCtx,
                                        const TrackingOptions &TrackOpts,
                                        Interpreter &I) {
  auto &SelfcheckSection = ProgCtx.getSelfcheckSection();
  std::vector<char> Data(SelfcheckSection.Size);
  I.readMem(SelfcheckSection.VMA, Data);

  const auto SCStride = ProgramConfig::getSCStride();
  auto ResultOffset = 0;
  auto ReferenceOffset = SCStride;
  auto ChunksNum = 2;

  size_t BlockSize = ChunksNum * SCStride;

  auto DataSize = Data.size() - Data.size() % BlockSize;
  for (size_t Offset = 0; Offset < DataSize; Offset += BlockSize) {
    auto It = Data.begin() + Offset;
    auto ResultIt = It + ResultOffset;
    auto ReferenceIt = It + ReferenceOffset;
    for (size_t ByteIdx = 0; ByteIdx < SCStride; ++ByteIdx) {
      auto Result = ResultIt[ByteIdx];
      auto Reference = ReferenceIt[ByteIdx];
      auto DefMaskByte = 0xFF;
      if ((Result & DefMaskByte) != (Reference & DefMaskByte)) {
        auto FaultAddr = SelfcheckSection.VMA + Offset + ByteIdx;
        LLVM_DEBUG(
            dumpSelfCheck(Data, BlockSize / ChunksNum, ChunksNum, dbgs()));
        snippy::fatal(formatv(
            "Incorrect memory state after interpretation in "
            "self-check mode. Error is in block @ 0x{0}{{1} + {2} + {3}}\n",
            Twine::utohexstr(FaultAddr), Twine::utohexstr(SelfcheckSection.VMA),
            Twine::utohexstr(Offset), Twine::utohexstr(ByteIdx)));
      }
    }
  }
}

static void dumpVerificationIntervalsIfNeeeded(SnippyModule &SM,
                                               const GeneratorContext &GenCtx,
                                               StringRef BaseFileName) {
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

  auto RegionsToVerifyFilename =
      RegionsToVerify.isSpecified() && RegionsToVerify.getValue().empty()
          ? addExtensionIfRequired(BaseFileName, ".intervals-to-verify.yaml")
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

GeneratorResult FlowGenerator::generate(LLVMState &State,
                                        const DebugOptions &DebugCfg) {

  auto RegGen =
      createRegGen(RegGeneratorFile.getValue(), RegInfoFile.getValue());

  SnippyProgramContext ProgContext(State, RegGen, RP, OpCC, *Cfg.ProgramCfg);

  const auto &SnippyTgt = State.getSnippyTarget();
  auto MainModule = SnippyModule(ProgContext.getLLVMState(), "main");

  GeneratorContext GenCtx(ProgContext, Cfg);
  auto &PassCfg = Cfg.PassCfg;

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
            PassCfg.RegistersConfig.InitializeRegs));
        SnippyTgt.addTargetSpecificPasses(PM);
        // Pre backtrack end

        PM.add(createBlockGenPlanWrapperPass());
        PM.add(createBlockGenPlanningPass());
        PM.add(createInstructionGeneratorPass()); // Can be backtracked

        // Post backtrack
        PM.add(createPrologueEpilogueInsertionPass());
        PM.add(createFillExternalFunctionsStubsPass({}));
        if (DebugCfg.DumpMF)
          PM.add(createMachineFunctionPrinterPass(outs()));

        if (DebugCfg.DumpMI)
          PM.add(createPrintMachineInstrsPass(outs()));

        SnippyTgt.addTargetLegalizationPasses(PM);
        PM.add(createBranchRelaxatorPass());
        if (VerifyConsecutiveLoops)
          PM.add(createConsecutiveLoopsVerifierPass());

        PM.add(createPostGenVerifierPass());
        PM.add(createInstructionsPostProcessPass());
        PM.add(createFunctionDistributePass());

        PM.add(createTrackLivenessPass());

        if (PassCfg.InstrsGenerationConfig.RunMachineInstrVerifier)
          PM.add(createMachineVerifierPass("Machine Verifier Pass report"));

        if (DebugCfg.DumpCFG)
          PM.add(createCFGPrinterPass(DebugCfg.ViewCFG));

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
  bool NoRelax = DisableLinkerRelaxations;
  auto EResult = ProgContext.generateELF(Modules, ObjectType, NoRelax);
  if (!EResult)
    snippy::fatal(EResult.takeError());

  dumpVerificationIntervalsIfNeeeded(MainModule, GenCtx, BaseFileName);

  if (PassCfg.ModelPluginConfig.runOnModel()) {
    auto GenType = ObjectType == GeneratorResult::Type::RELOC
                       ? GeneratorResult::Type::LEGACY_EXEC
                       : ObjectType;
    auto ESnippetImageForModelExecution =
        ProgContext.generateELF(Modules, GenType, NoRelax);
    if (!ESnippetImageForModelExecution)
      snippy::fatal(ESnippetImageForModelExecution.takeError());

    auto RI = SimulatorContext::RunInfo{
        ESnippetImageForModelExecution->SnippetImage, ProgContext, MainModule,
        PassCfg.ProgramCfg.EntryPointName,
        PassCfg.RegistersConfig.InitialStateOutputYaml,
        PassCfg.RegistersConfig.FinalStateOutputYaml, SelfCheckMem,
        // Memory reset only needed if interpreter may have executed
        // during generation process.
        /* NeedMemoryReset */ Cfg.hasTrackingMode(),
        std::vector<std::string>(DumpMemorySection.begin(),
                                 DumpMemorySection.end()),
        MemorySectionFile.getValue(), BaseFileName};

    auto &SimCtx = MainModule.getGenResult<OwningSimulatorContext>();

    // TODO: move this all to some more simulator-related place
    auto &I = SimCtx.getInterpreter();
    std::unique_ptr<RVMCallbackHandler::ObserverHandle<SelfcheckObserver>>
        SelfcheckObserverHandle;
    auto &TrackCfg = Cfg.getTrackCfg();
    if (TrackCfg.SelfCheckPeriod) {
      auto &Map = MainModule.getOrAddResult<SelfCheckMap>().Map;
      // TODO: merge all infos from all modules.
      SelfcheckObserverHandle =
          I.setObserver<SelfcheckObserver>(Map.begin(), Map.end(), I.getPC());
    }

    if (auto Err = SimCtx.runSimulator(RI))
      snippy::fatal(GenCtx.getProgramContext().getLLVMState().getCtx(),
                    "Error during the simulation run", std::move(Err));

    if (TrackCfg.SelfCheckPeriod) {
      if (SelfCheckMem)
        checkMemStateAfterSelfcheck(ProgContext, TrackCfg, I);

      auto AnnotationFilename =
          addExtensionIfRequired(BaseFileName, ".selfcheck.yaml");
      I.getObserverByHandle(*SelfcheckObserverHandle)
          .dumpAsYaml(AnnotationFilename);
    }

  } else {

    snippy::warn(WarningName::NoModelExec, State.getCtx(),
                 "Skipping snippet execution on the model",
                 "model was set no 'None'.");
  }
  if (EResult->GenType == GeneratorResult::Type::RELOC)
    snippy::notice(
        WarningName::RelocatableGenerated, State.getCtx(),
        "Snippet generator generated relocatable image",
        "please, use linker with provided script to generate final image");

  return *EResult;
}

} // namespace snippy
} // namespace llvm
