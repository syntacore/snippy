//===-- SnippyModule.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SnippyModule.h"
#include "snippy/Generator/GeneratorSettings.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/InitializePasses.h"

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

namespace llvm {
namespace snippy {

SnippyModule::SnippyModule(LLVMState &State, StringRef Name)
    : State(State), M(Name, State.getCtx()), Context([&]() {
        auto &LLVMTM = State.getTargetMachine();
        // Previously, AsmPrinter was created using Context from MMI
        // MMI is captured by PM, so in order to avoid potential invalid ref,
        //  now MMIWP uses external context
        auto Context = std::make_unique<MCContext>(
            LLVMTM.getTargetTriple(), LLVMTM.getMCAsmInfo(),
            LLVMTM.getMCRegisterInfo(), LLVMTM.getMCSubtargetInfo(), nullptr,
            &LLVMTM.Options.MCOptions, false);
        Context->setObjectFileInfo(LLVMTM.getObjFileLowering());
        return Context;
      }()),
      MMIWP(std::make_unique<MachineModuleInfoWrapperPass>(
          &State.getTargetMachine(), Context.get())),
      MMI(MMIWP->getMMI()) {
  M.setDataLayout(State.getTargetMachine().createDataLayout());
}

void SnippyModule::generateObject(const PassInserter &BeforePrinter,
                                  const PassInserter &AfterPrinter) {
  struct InitPasses {
    InitPasses(LLVMState &State) {
      const auto &SnippyTgt = State.getSnippyTarget();
      initializeCodeGen(*PassRegistry::getPassRegistry());
      initializeSnippyPasses(*PassRegistry::getPassRegistry());
      SnippyTgt.initializeTargetPasses();
    }
  };

  // Initialize passes only once.
  static InitPasses IP{State};

  // PassManager should live as long as SnippyModule for
  // MachineModuleInfo to stay alive.
  PPM = std::make_unique<PassManagerWrapper>();

  PPM->add(MMIWP.release());

  std::invoke(BeforePrinter, *PPM);

  GeneratedObject.clear();
  raw_svector_ostream OS(GeneratedObject);

  auto ObjStreamer = State.createObjStreamer(OS, *Context);
  auto AsmPrinter = State.getSnippyTarget().createAsmPrinter(
      State.getTargetMachine(), std::move(ObjStreamer));
  PPM->add(AsmPrinter.release());
  std::invoke(AfterPrinter, *PPM);
  PPM->run(M);

  outs().flush(); // FIXME: this is currently needed because
                  //        MachineFunctionPrinter don't flush
}

void SnippyProgramContext::initializeROMSection(
    const SnippyProgramSettings &Settings) {
  if (!getLinker().sections().hasOutputSectionFor(
          Linker::kDefaultRODataSectionName))
    return;
  ROMSection = getLinker()
                   .sections()
                   .getOutputSectionFor(Linker::kDefaultRODataSectionName)
                   .Desc;
}

void SnippyProgramContext::initializeUtilitySection(
    const SnippyProgramSettings &Settings) {
  auto &Ctx = State->getCtx();
  if (!Settings.Sections.hasSection(SectionsDescriptions::UtilitySectionName))
    return;
  UtilitySection =
      Settings.Sections.getSection(SectionsDescriptions::UtilitySectionName);
  auto AccMask = UtilitySection->M;
  if (!(AccMask.R() && AccMask.W() && !AccMask.X()))
    snippy::fatal(Ctx, "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::UtilitySectionName) +
                      "\" section must be RW");
}

void SnippyProgramContext::initializeSelfcheckSection(
    const SnippyProgramSettings &Settings) {
  if (!Settings.Sections.hasSection(SectionsDescriptions::SelfcheckSectionName))
    return;
  // Configure selfcheck from layout.
  SelfcheckSection =
      Settings.Sections.getSection(SectionsDescriptions::SelfcheckSectionName);
}

GeneratorResult SnippyProgramContext::generateELF(
    ArrayRef<const SnippyModule *> Modules) const {
  GeneratorResult Result;
  assert(llvm::all_of(
      Modules, [](auto &Mapped) { return Mapped->haveGeneratedObject(); }));

  ObjectFilesList Objects;
  std::transform(Modules.begin(), Modules.end(), std::back_inserter(Objects),
                 [](auto &Mapped) { return Mapped->getGeneratedObject(); });

  Result.SnippetImage = PLinker->run(Objects, /*Relocatable*/ true);
  Result.LinkerScript = PLinker->generateLinkerScript();

  return Result;
}

std::string SnippyProgramContext::generateLinkedImage(
    ArrayRef<const SnippyModule *> Modules) const {
  assert(llvm::all_of(
      Modules, [](auto &Mapped) { return Mapped->haveGeneratedObject(); }));

  ObjectFilesList Objects;
  std::transform(Modules.begin(), Modules.end(), std::back_inserter(Objects),
                 [](auto &Mapped) { return Mapped->getGeneratedObject(); });

  return PLinker->run(Objects, /*Relocatable*/ false);
}

bool SnippyProgramContext::shouldSpillStackPointer() const {
  if (!followTargetABI())
    return false;
  auto RealStackPointer = getStackPointer();
  const auto &SnippyTgt = getLLVMState().getSnippyTarget();
  auto ABIPreservedRegs = SnippyTgt.getRegsPreservedByABI();
  return std::any_of(ABIPreservedRegs.begin(), ABIPreservedRegs.end(),
                     [RealStackPointer](auto PreservedReg) {
                       return PreservedReg == RealStackPointer;
                     });
}

void SnippyProgramContext::initializeStackSection(
    const SnippyProgramSettings &Settings) {
  if (!Settings.Sections.hasSection(SectionsDescriptions::StackSectionName))
    return;
  auto &Ctx = State->getCtx();
  auto &SnippyTgt = State->getSnippyTarget();
  auto SP = getStackPointer();
  auto Align = SnippyTgt.getSpillAlignmentInBytes(SP, *State);

  // Configure stack from layout.
  StackSection =
      Settings.Sections.getSection(SectionsDescriptions::StackSectionName);
  auto M = StackSection.value().M;
  if (!(M.R() && M.W() && !M.X()))
    snippy::fatal(Ctx, "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::StackSectionName) +
                      "\" section must be RW");
  if (ExternalStack) {
    if (RegPoolsStorage.front().isReserved(getStackPointer()))
      snippy::fatal(State->getCtx(), "Cannot configure external stack",
                    "stack pointer register is "
                    "explicitly reserved.");
  }

  if (StackSection) {
    if (StackSection->VMA % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section VMA must be " + Twine(Align) +
                        " bytes aligned.");
    if (StackSection->Size % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section size must be " + Twine(Align) +
                        " bytes aligned.");

    if (RegPoolsStorage.front().isReserved(SP))
      snippy::fatal(Ctx, "Failed to initialize stack",
                    "stack pointer register is "
                    "explicitly reserved.");
  }
}

SnippyProgramContext::SnippyProgramContext(
    LLVMState &State, RegisterGenerator &RegGen, RegPool &Pool,
    const OpcodeCache &OpCc, const SnippyProgramSettings &Settings)
    : State(&State), RegGen(&RegGen), RegPoolsStorage({Pool}), OpCC(&OpCc),
      PLinker(std::make_unique<Linker>(
          State.getCtx(), Settings.Sections,
          Settings.MangleExportedNames ? Settings.EntryPointName : "")),
      StackPointer(Settings.StackPointer),
      MangleExportedNames(Settings.MangleExportedNames),
      EntryPointName(Settings.EntryPointName),
      ExternalStack(Settings.ExternalStack),
      FollowTargetABI(Settings.FollowTargetABI) {

  initializeStackSection(Settings);
  initializeSelfcheckSection(Settings);
  initializeUtilitySection(Settings);
  initializeROMSection(Settings);
}

SnippyProgramContext::~SnippyProgramContext() = default;

} // namespace snippy
} // namespace llvm
