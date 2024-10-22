//===-- FlowGenerator.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FlowGenerator.h"
#include "BlockGenPlanWrapperPass.h"
#include "BlockGenPlanningPass.h"
#include "InitializePasses.h"

#include "snippy/Config/Branchegram.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/CallGraphState.h"
#include "snippy/Generator/Generation.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/IntervalsToVerify.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/RegisterPool.h"
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

#define DEBUG_TYPE "snippy-flow-generator"
#define PASS_DESC "Snippy Flow Generator"

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

struct CallGraphDumpEnumOption
    : public snippy::EnumOptionMixin<CallGraphDumpEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(CallGraphDumpMode::Dot, "dot",
                    "is used to render visual graph representation");
    Mapper.enumCase(CallGraphDumpMode::Yaml, "yaml",
                    "can be read back by snippy");
  }
};

static snippy::opt<CallGraphDumpMode>
    CGDumpFormat("call-graph-dump-format",
                 cl::desc("Choose format for call graph dump option:"),
                 CallGraphDumpEnumOption::getClValues(),
                 cl::init(CallGraphDumpMode::Dot), cl::cat(Options));

snippy::opt<bool> VerifyConsecutiveLoops(
    "verify-consecutive-loops",
    cl::desc(
        "Check that consecutive loops generated accordingly to branchegram."),
    cl::cat(Options), cl::init(false), cl::Hidden);

} // namespace snippy

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::CallGraphDumpMode,
                                           snippy::CallGraphDumpEnumOption)

namespace snippy {

static snippy::opt<std::string>
    DumpCGFilename("call-graph-dump-filename",
                   cl::desc("Specify file to dump call graph in dot format"),
                   cl::value_desc("filename"), cl::init(""), cl::cat(Options));
static snippy::opt<bool> SelfCheckGV(
    "selfcheck-gv",
    cl::desc("add selfcheck section properties such as VMA, size and stride as "
             "a global constants with an external linkage"),
    cl::Hidden, cl::init(false));
static snippy::opt<bool> ExportGV(
    "export-gv",
    cl::desc(
        "add sections properties such as VMA, size and stride as "
        "a global constants with an external linkage. Requires a model plugin"),
    cl::cat(Options), cl::init(false));

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

class InstructionGenerator final : public MachineFunctionPass {
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
  SelfCheckInfo SelfCheckInfo{};

public:
  static char ID;

  InstructionGenerator() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<BlockGenPlanWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

char InstructionGenerator::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::InstructionGenerator;

INITIALIZE_PASS_BEGIN(InstructionGenerator, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(BlockGenPlanWrapper)
INITIALIZE_PASS_END(InstructionGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createInstructionGeneratorPass() {
  return new InstructionGenerator();
}

namespace snippy {
// This must always be in sync with prologue epilogue insertion.
static size_t calcMainFuncInitialSpillSize(GeneratorContext &GC) {
  auto &State = GC.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto StackPointer = GC.getProgramContext().getStackPointer();
  size_t SpillSize = SnippyTgt.getSpillAlignmentInBytes(StackPointer, State);
  // We'll spill a register we use as a stack pointer.
  if (GC.getProgramContext().shouldSpillStackPointer())
    SpillSize += SnippyTgt.getSpillSizeInBytes(StackPointer, GC);

  auto SpilledRef = GC.getRegsSpilledToStack();
  std::vector SpilledRegs(SpilledRef.begin(), SpilledRef.end());
  llvm::copy(GC.getRegsSpilledToMem(), std::back_inserter(SpilledRegs));
  return std::accumulate(SpilledRegs.begin(), SpilledRegs.end(), SpillSize,
                         [&GC, &SnippyTgt](auto Init, auto Reg) {
                           return Init + SnippyTgt.getSpillSizeInBytes(Reg, GC);
                         });
}

void InstructionGenerator::prepareInterpreterEnv() const {
  auto &State = SGCtx->getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &ProgCtx = SGCtx->getProgramContext();
  auto &I = SGCtx->getOrCreateInterpreter();

  I.setInitialState(SGCtx->getInitialRegisterState(I.getSubTarget()));
  if (!ProgCtx.hasStackSection())
    return;

  // Prologue insertion happens after instructions generation, so we do not
  // have SP initialization instructions at this point. However, we know the
  // actual value of SP, so let's initialize it in model artificially.
  auto SP = ProgCtx.getStackPointer();
  APInt StackTop(SnippyTgt.getRegBitWidth(SP, *SGCtx),
                 ProgCtx.getStackTop() - calcMainFuncInitialSpillSize(*SGCtx));
  I.setReg(SP, StackTop);
}

void InstructionGenerator::addGV(
    const APInt &Value, unsigned long long Stride = 1,
    GlobalValue::LinkageTypes LType = GlobalValue::ExternalLinkage,
    StringRef Name = "global") const {
  auto &GP = SGCtx->getGlobalsPool();

  auto *GV = GP.createGV(Value, Stride, LType, Name);
  if (SGCtx->hasTrackingMode())
    SGCtx->getOrCreateInterpreter().writeMem(GP.getGVAddress(GV), Value);
}

void InstructionGenerator::addModelMemoryPropertiesAsGV() const {
  auto MemCfg = MemoryConfig::getMemoryConfig(*SGCtx);
  // Below we add all the model memory properties as global constants
  constexpr auto ConstantSizeInBits = 64u; // Constants size in bits
  constexpr auto Alignment = 1u;           // Without special alignment

  auto DataSectionVMA = APInt{ConstantSizeInBits, MemCfg.Ram.Start};
  addGV(DataSectionVMA, Alignment, GlobalValue::ExternalLinkage,
        "data_section_address");

  auto DataSectionSize = APInt{ConstantSizeInBits, MemCfg.Ram.Size};
  addGV(DataSectionSize, Alignment, GlobalValue::ExternalLinkage,
        "data_section_size");

  auto ExecSectionVMA =
      APInt{ConstantSizeInBits, MemCfg.ProgSections.front().Start};
  addGV(ExecSectionVMA, Alignment, GlobalValue::ExternalLinkage,
        "exec_section_address");

  auto ExecSectionSize =
      APInt{ConstantSizeInBits, MemCfg.ProgSections.front().Size};
  addGV(ExecSectionSize, Alignment, GlobalValue::ExternalLinkage,
        "exec_section_size");
}

void InstructionGenerator::addSelfcheckSectionPropertiesAsGV() const {
  const auto &SelfcheckSection = SGCtx->getSelfcheckSection();

  auto VMA = APInt{64, SelfcheckSection.VMA};
  auto Size = APInt{64, SelfcheckSection.Size};
  auto Stride = APInt{64, SGCtx->getProgramContext().getSCStride()};

  addGV(VMA, 1, GlobalValue::ExternalLinkage, "selfcheck_section_address");
  addGV(Size, 1, GlobalValue::ExternalLinkage, "selfcheck_section_size");
  addGV(Stride, 1, GlobalValue::ExternalLinkage, "selfcheck_data_byte_stride");
}

planning::FunctionRequest InstructionGenerator::createMFGenerationRequest(
    const MachineFunction &MF) const {
  auto &FunReq = getAnalysis<BlockGenPlanWrapper>().getFunctionRequest(&MF);
  const auto &ProgCtx = SGCtx->getProgramContext();
  const MCInstrDesc *FinalInstDesc = nullptr;
  auto LastInstrStr = SGCtx->getLastInstr();
  if (!LastInstrStr.empty() && !SGCtx->useRetAsLastInstr()) {
    auto Opc = ProgCtx.getOpcodeCache().code(LastInstrStr.str());
    if (!Opc.has_value())
      snippy::fatal("unknown opcode \"" + Twine(LastInstrStr) +
                    "\" for last instruction generation");
    FinalInstDesc = ProgCtx.getOpcodeCache().desc(Opc.value());
  }
  FunReq.setFinalInstr(FinalInstDesc);
  return FunReq;
}

static void reserveAddressesForRegSpills(ArrayRef<MCRegister> Regs,
                                         GeneratorContext &GC) {
  llvm::for_each(Regs, [&](auto Reg) { GC.reserveSpillAddrsForReg(Reg); });
}

bool InstructionGenerator::runOnMachineFunction(MachineFunction &MF) {
  SGCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &GenSettings = SGCtx->getGenSettings();
  if (SGCtx->getConfig().hasSectionToSpillGlobalRegs() &&
      (SGCtx->getSpilledRegAddressesGlobal().empty() ||
       SGCtx->getSpilledRegAddressesLocal().empty()))
    reserveAddressesForRegSpills(GenSettings.RegistersConfig.SpilledToMem,
                                 *SGCtx);
  if (SGCtx->hasTrackingMode())
    prepareInterpreterEnv();

  if (ExportGV)
    addModelMemoryPropertiesAsGV();

  if (GenSettings.TrackingConfig.SelfCheckPeriod) {
    SelfCheckInfo.PeriodTracker = {GenSettings.TrackingConfig.SelfCheckPeriod};
    const auto &SCSection = SGCtx->getSelfcheckSection();
    SelfCheckInfo.CurrentAddress = SCSection.VMA;
    // FIXME: make SelfCheckGV a deprecated option
    if (SelfCheckGV || ExportGV) {
      if (!ExportGV)
        addModelMemoryPropertiesAsGV();
      addSelfcheckSectionPropertiesAsGV();
    }
  }

  auto FunctionGenRequest = createMFGenerationRequest(MF);
  generate(FunctionGenRequest, MF, *SGCtx, &SelfCheckInfo,
           &getAnalysis<MachineLoopInfo>());
  return true;
}

static void dumpVerificationIntervalsIfNeeeded(StringRef Output,
                                               const GeneratorContext &GenCtx) {
  if (!RegionsToVerify.isSpecified())
    return;

  auto &State = GenCtx.getLLVMState();
  auto &Ctx = State.getCtx();

  auto VerificationIntervals = IntervalsToVerify::createFromObject(
      State.getDisassembler(), Output,
      GenCtx.getProgramContext().getEntryPointName(),
      GenCtx.getLinker().sections().getOutputSectionFor(".text").Desc.VMA,
      GenCtx.getEntryPrologueInstructionCount(),
      GenCtx.getEntryEpilogueInstructionCount());

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

  GeneratorContext GenCtx(ProgContext, GenSettings);
  GenCtx.attachTargetContext(SnippyTgt.createTargetContext(GenCtx));

  std::string MIR;
  raw_string_ostream MIROS(MIR);

  auto &MainModule = GenCtx.getMainModule();

  MainModule.generateObject(
      [&](PassManagerWrapper &PM) {
        // Pre backtrack start
        PM.add(createGeneratorContextWrapperPass(GenCtx));
        PM.add(createRootRegPoolWrapperPass());
        PM.add(createFunctionGeneratorPass());

        PM.add(createReserveRegsPass());
        PM.add(createCFGeneratorPass());
        if (GenSettings.Cfg.Branches.PermuteCF)
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
      },
      [](PassManagerWrapper &PM) {});

  auto CGFilename = DumpCGFilename.getValue();
  if (!CGFilename.empty())
    GenCtx.getCallGraphState().dump(CGFilename, CGDumpFormat);

  if (DumpMIR.isSpecified())
    writeMIRFile(MIR);
  dumpMemAccessesIfNeeded(GenCtx);
  std::vector<const SnippyModule *> Modules{&GenCtx.getMainModule()};
  auto Result = ProgContext.generateELF(Modules);

  dumpVerificationIntervalsIfNeeeded(
      GenCtx.getMainModule().getGeneratedObject(), GenCtx);

  if (GenSettings.ModelPluginConfig.RunOnModel) {
    auto SnippetImageForModelExecution =
        ProgContext.generateLinkedImage(Modules);

    GenCtx.runSimulator(SnippetImageForModelExecution);
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
