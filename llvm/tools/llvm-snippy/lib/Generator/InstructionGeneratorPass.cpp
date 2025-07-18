//===-- InstructionGeneratorPass.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../InitializePasses.h"

#include "snippy/Generator/BlockGenPlanWrapperPass.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/InstructionGeneratorPass.h"
#include "snippy/Generator/LoopLatcherPass.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Generator/SnippyFunctionMetadata.h"
#include "snippy/Support/Options.h"

#include "llvm/CodeGen/MachineLoopInfo.h"

#define DEBUG_TYPE "snippy-flow-generator"
#define PASS_DESC "Snippy Flow Generator"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<bool> SelfCheckGV(
    "selfcheck-gv",
    cl::desc("add selfcheck section properties such as VMA, size and stride as "
             "a global constants with an external linkage"),
    cl::Hidden, cl::init(false));

char InstructionGenerator::ID = 0;
StringRef InstructionGenerator::getPassName() const {
  return PASS_DESC " Pass";
}

} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::InstructionGenerator;

SNIPPY_INITIALIZE_PASS(InstructionGenerator, DEBUG_TYPE, PASS_DESC, false)

namespace llvm {

snippy::ActiveImmutablePassInterface *createInstructionGeneratorPass() {
  return new snippy::InstructionGenerator();
}

namespace snippy {
void InstructionGenerator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<SnippyFunctionMetadataWrapper>();
  AU.addRequired<SimulatorContextWrapper>();
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addRequired<FunctionGenerator>();
  AU.addRequired<BlockGenPlanWrapper>();
  AU.addRequired<LoopLatcher>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

// This must always be in sync with prologue epilogue insertion.
static size_t calcMainFuncInitialSpillSize(InstructionGenerationContext &IGC) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();

  auto StackPointer = ProgCtx.getStackPointer();
  size_t SpillSize = SnippyTgt.getSpillAlignmentInBytes(StackPointer, State);
  // We'll spill a register we use as a stack pointer.
  if (ProgCtx.shouldSpillStackPointer())
    SpillSize += SnippyTgt.getSpillSizeInBytes(StackPointer, IGC);

  const auto &Cfg = IGC.getCommonCfg();
  const auto &ProgCfg = Cfg.ProgramCfg;
  auto SpilledRef = ProgCfg.getRegsSpilledToStack();
  std::vector SpilledRegs(SpilledRef.begin(), SpilledRef.end());
  llvm::copy(ProgCfg.getRegsSpilledToMem(), std::back_inserter(SpilledRegs));
  return std::accumulate(SpilledRegs.begin(), SpilledRegs.end(), SpillSize,
                         [&IGC, &SnippyTgt](auto Init, auto Reg) {
                           return Init +
                                  SnippyTgt.getSpillSizeInBytes(Reg, IGC);
                         });
}

void InstructionGenerator::prepareInterpreterEnv(MachineFunction &MF) const {
  const auto &ProgCtx = SGCtx->getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &I = SimCtx.getInterpreter();

  // FIXME: it is too late to prepare interpreter env here. Some passes may
  // have already modified memory. Better to do that before all that passes.
  // Currently FullReset == false is introduced to fix current issues with it.
  I.resetState(ProgCtx,
               /*FullReset*/ false);

  if (!ProgCtx.hasStackSection())
    return;

  // Prologue insertion happens after instructions generation, so we do not
  // have SP initialization instructions at this point. However, we know the
  // actual value of SP, so let's initialize it in model artificially.
  auto SP = ProgCtx.getStackPointer();
  assert(!MF.empty());
  InstructionGenerationContext IGC{MF.front(), MF.front().begin(), *SGCtx,
                                   SimCtx};
  APInt StackTop(SnippyTgt.getRegBitWidth(SP, IGC),
                 ProgCtx.getStackTop() - calcMainFuncInitialSpillSize(IGC));
  I.setReg(SP, StackTop);
}

void InstructionGenerator::addGV(
    Module &M, const APInt &Value, unsigned long long Stride = 1,
    GlobalValue::LinkageTypes LType = GlobalValue::ExternalLinkage,
    StringRef Name = "global") const {
  auto &ProgCtx = SGCtx->getProgramContext();
  auto &GP = ProgCtx.getOrAddGlobalsPoolFor(
      SnippyModule::fromModule(M),
      "Failed to allocate space for selfcheck global data");

  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  auto *GV = GP.createGV(Value, Stride, LType, Name);
  if (SimCtx.hasTrackingMode())
    SimCtx.getInterpreter().writeMem(GP.getGVAddress(GV), Value);
}

void InstructionGenerator::addSelfcheckSectionPropertiesAsGV(Module &M) const {
  const auto &SelfcheckSection =
      SGCtx->getProgramContext().getSelfcheckSection();

  auto VMA = APInt{64, SelfcheckSection.VMA};
  auto Size = APInt{64, SelfcheckSection.Size};
  auto Stride = APInt{64, ProgramConfig::getSCStride()};

  addGV(M, VMA, 1, GlobalValue::ExternalLinkage, "selfcheck_section_address");
  addGV(M, Size, 1, GlobalValue::ExternalLinkage, "selfcheck_section_size");
  addGV(M, Stride, 1, GlobalValue::ExternalLinkage,
        "selfcheck_data_byte_stride");
}

planning::FunctionRequest InstructionGenerator::createMFGenerationRequest(
    const MachineFunction &MF) const {
  auto &FunReq = getAnalysis<BlockGenPlanWrapper>().getFunctionRequest(&MF);
  const auto &ProgCtx = SGCtx->getProgramContext();
  const MCInstrDesc *FinalInstDesc = nullptr;
  const auto &PassCfg = SGCtx->getConfig().PassCfg;
  StringRef LastInstrStr = PassCfg.InstrsGenerationConfig.LastInstr;
  if (!LastInstrStr.empty() &&
      !PassCfg.InstrsGenerationConfig.useRetAsLastInstr()) {
    auto Opc = ProgCtx.getOpcodeCache().code(LastInstrStr.str());
    if (!Opc.has_value())
      snippy::fatal("unknown opcode \"" + Twine(LastInstrStr) +
                    "\" for last instruction generation");
    FinalInstDesc = ProgCtx.getOpcodeCache().desc(Opc.value());
  }
  FunReq.setFinalInstr(FinalInstDesc);
  return FunReq;
}

// FIXME: this should be moved in some early module wide pass.
static void reserveAddressesForRegSpills(ArrayRef<MCRegister> Regs,
                                         GeneratorContext &GC,
                                         MachineFunction &MF) {
  if (Regs.empty())
    return;
  auto &ProgCtx = GC.getProgramContext();
  assert(ProgCtx.hasProgramStateSaveSpace());
  auto &SaveLocs = ProgCtx.getProgramStateSaveSpace();
  auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  SimulatorContext SimCtx{};
  InstructionGenerationContext IGC{MF.front(), MF.front().begin(), GC, SimCtx};
  llvm::for_each(Regs, [&](auto Reg) {
    if (SaveLocs.hasSaveLocation(Reg))
      return;
    SaveLocs.allocateSaveLocation(Reg, SnippyTgt.getRegBitWidth(Reg, IGC) /
                                           CHAR_BIT);
  });
}

bool InstructionGenerator::runOnMachineFunction(MachineFunction &MF) {
  SGCtx = &getAnalysis<GeneratorContextWrapper>().getContext();
  auto &SimCtx = getAnalysis<SimulatorContextWrapper>()
                     .get<OwningSimulatorContext>()
                     .get();
  const auto &Cfg = SGCtx->getConfig();
  auto &ProgCfg = *Cfg.ProgramCfg;
  if (ProgCfg.hasSectionToSpillGlobalRegs())
    reserveAddressesForRegSpills(ProgCfg.SpilledToMem, *SGCtx, MF);
  if (SimCtx.hasTrackingMode())
    prepareInterpreterEnv(MF);

  auto &M = *MF.getFunction().getParent();

  auto *SCI = SimCtx.SCI;
  if (SCI) {
    // TODO: move it to initializer:
    SCI->PeriodTracker = {Cfg.getTrackCfg().SelfCheckPeriod};
    const auto &SCSection = SGCtx->getProgramContext().getSelfcheckSection();
    SCI->CurrentAddress = SCSection.VMA;
    // FIXME: make SelfCheckGV a deprecated option
    if (SelfCheckGV)
      addSelfcheckSectionPropertiesAsGV(M);
  }

  auto FunctionGenRequest = createMFGenerationRequest(MF);
  generate(FunctionGenRequest, MF, *SGCtx, SimCtx,
           &getAnalysis<MachineLoopInfoWrapperPass>().getLI(),
           &getAnalysis<FunctionGenerator>().getCallGraphState(),
           &get<MemAccessInfo>(MF),
           &getAnalysis<LoopLatcher>().get<SnippyLoopInfo>(MF),
           &getAnalysis<SnippyFunctionMetadataWrapper>().get(MF));
  return true;
}

} // namespace snippy
} // namespace llvm
