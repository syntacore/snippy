//===-- SnippyModule.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/SnippyModule.h"
#include "snippy/Config/Config.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/InitializePasses.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
namespace llvm {
namespace snippy {

template class GenResultT<ObjectFile>;
template class GenResultT<ObjectMetadata>;

SnippyModule::SnippyModule(LLVMState &State, StringRef Name)
    : Module(Name, State.getCtx()), State(State), Context([&]() {
        setTargetTriple(State.getSubtargetInfo().getTargetTriple().getTriple());
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
  setDataLayout(State.getTargetMachine().createDataLayout());
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

  SmallString<32> GeneratedObject;
  raw_svector_ostream OS(GeneratedObject);

  auto ObjStreamer = State.createObjStreamer(OS, *Context);
  auto AsmPrinter = State.getSnippyTarget().createAsmPrinter(
      State.getTargetMachine(), std::move(ObjStreamer));
  PPM->add(AsmPrinter.release());
  std::invoke(AfterPrinter, *PPM);
  PPM->run(getModule());
  addGenResult<ObjectFile>(std::move(GeneratedObject));

  outs().flush(); // FIXME: this is currently needed because
                  //        MachineFunctionPrinter don't flush
}

void SnippyProgramContext::initializeROMSection(const ProgramConfig &Settings) {
  if (!getLinker().sections().hasOutputSectionFor(
          Linker::kDefaultRODataSectionName))
    return;
  ROMSection = getLinker()
                   .sections()
                   .getOutputSectionFor(Linker::kDefaultRODataSectionName)
                   .Desc;
  if (ROMSection)
    ROMSectionManager = std::make_unique<MonoAllocatableSection>(*ROMSection);
}

void SnippyProgramContext::initializeUtilitySection(
    const ProgramConfig &Settings) {
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
  PGSK = std::make_unique<ProgramGlobalStateKeeper>(*State, *UtilitySection);
}

Expected<GlobalsPool &>
SnippyProgramContext::getOrAddGlobalsPoolFor(Module &M) {
  auto *Key = &M;
  if (PerModuleGPs.count(Key))
    return *PerModuleGPs.at(Key);
  if (!ROMSection)
    return makeFailure(Errc::InvalidConfiguration,
                       "ROM section is not configured");

  return *PerModuleGPs
              .emplace(Key, std::make_unique<GlobalsPool>(
                                *State, M, *ROMSectionManager,
                                exportedNamesMangled()
                                    ? ("__snippy_" +
                                       Twine(getEntryPointName()) + "_")
                                          .str()
                                    : "__snippy_"))
              .first->second;
}

Expected<GlobalsPool &>
SnippyProgramContext::getOrAddGlobalsPoolFor(SnippyModule &M) {
  return getOrAddGlobalsPoolFor(M.getModule());
}

GlobalsPool &SnippyProgramContext::getOrAddGlobalsPoolFor(Module &M,
                                                          StringRef OnError) {
  auto EPool = getOrAddGlobalsPoolFor(M);
  if (EPool)
    return EPool.get();
  auto E = EPool.takeError();
  snippy::fatal(OnError, [&]() {
    std::string Msg;
    raw_string_ostream OS{Msg};
    OS << E;
    return Msg;
  }());
}

GlobalsPool &SnippyProgramContext::getOrAddGlobalsPoolFor(SnippyModule &M,
                                                          StringRef OnError) {
  return getOrAddGlobalsPoolFor(M.getModule(), OnError);
}

void SnippyProgramContext::initializeSelfcheckSection(
    const ProgramConfig &Settings) {
  if (!Settings.Sections.hasSection(SectionsDescriptions::SelfcheckSectionName))
    return;
  // Configure selfcheck from layout.
  SelfcheckSection =
      Settings.Sections.getSection(SectionsDescriptions::SelfcheckSectionName);
}

Expected<GeneratorResult>
SnippyProgramContext::generateELF(ArrayRef<const SnippyModule *> Modules,
                                  GeneratorResult::Type GenType,
                                  bool NoRelax) const {
  GeneratorResult Result;

  assert(llvm::all_of(
      Modules, [](auto &Mapped) { return Mapped->haveGeneratedObject(); }));

  ObjectFilesList Objects;
  std::transform(Modules.begin(), Modules.end(), std::back_inserter(Objects),
                 [](auto &Mapped) { return Mapped->getGeneratedObject(); });
  auto UseLegacy = GenType == GeneratorResult::Type::RELOC ||
                   GenType == GeneratorResult::Type::LEGACY_EXEC;
  if (UseLegacy)
    Result.SnippetImage = PLinker->runLegacy(
        Objects, GenType == GeneratorResult::Type::RELOC, NoRelax);
  else {
    auto EImage = PLinker->run(Objects, GenType == GeneratorResult::Type::DYN);
    if (!EImage)
      return EImage.takeError();
    Result.SnippetImage = *EImage;
  }
  if (GenType == GeneratorResult::Type::RELOC)
    Result.LinkerScript = PLinker->generateLinkerScript();
  Result.GenType = GenType;
  return Result;
}

bool SnippyProgramContext::shouldSpillStackPointer() const {
  if (!followTargetABI())
    return false;
  auto RealStackPointer = getStackPointer();
  const auto &SnippyTgt = getLLVMState().getSnippyTarget();
  auto ABIPreservedRegs =
      SnippyTgt.getRegsPreservedByABI(State->getSubtargetInfo());
  return std::any_of(ABIPreservedRegs.begin(), ABIPreservedRegs.end(),
                     [RealStackPointer](auto PreservedReg) {
                       return PreservedReg == RealStackPointer;
                     });
}

void SnippyProgramContext::initializeStackSection(
    const ProgramConfig &Settings) {
  if (ExternalStack) {
    if (getRegisterPool().isReserved(getStackPointer()))
      snippy::fatal(State->getCtx(), "Cannot configure external stack",
                    "stack pointer register is "
                    "explicitly reserved.");
    return;
  }
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

  if (StackSection) {
    if (StackSection->VMA % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section VMA must be " + Twine(Align) +
                        " bytes aligned.");
    if (StackSection->Size % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section size must be " + Twine(Align) +
                        " bytes aligned.");

    if (getRegisterPool().isReserved(SP))
      snippy::fatal(Ctx, "Failed to initialize stack",
                    "stack pointer register is "
                    "explicitly reserved.");
  }
}
const IRegisterState &SnippyProgramContext::getInitialRegisterState(
    const TargetSubtargetInfo &ST) const {
  if (InitialMachineState)
    return *InitialMachineState;
  InitialMachineState =
      State->getSnippyTarget().createRegisterState(getTargetContext(), ST);

  if (!InitialRegYamlFile.empty()) {
    WarningsT YamlWarnings;
    InitialMachineState->loadFromYamlFile(InitialRegYamlFile, YamlWarnings,
                                          &State->getSnippyTarget());
    std::for_each(YamlWarnings.begin(), YamlWarnings.end(), [&](StringRef Msg) {
      warn(WarningName::RegState, State->getCtx(), "register state yaml", Msg);
    });
  } else {
    InitialMachineState->randomize();
  }

  return *InitialMachineState;
}

SnippyProgramContext::SnippyProgramContext(LLVMState &State,
                                           RegisterGenerator &RegGen,
                                           std::vector<RegPool> Pools,
                                           const OpcodeCache &OpCc,
                                           const ProgramConfig &Settings)
    : Cfg(&Settings), State(&State), RegGen(&RegGen),
      RegPoolsStorage(std::move(Pools)), OpCC(&OpCc),
      PLinker(std::make_unique<Linker>(
          State.getCtx(), Settings.Sections,
          Settings.MangleExportedNames ? Settings.EntryPointName : "")),
      StackPointer(Settings.StackPointer),
      MangleExportedNames(Settings.MangleExportedNames),
      EntryPointName(Settings.EntryPointName),
      ExternalStack(Settings.ExternalStack),
      FollowTargetABI(Settings.FollowTargetABI),
      PreserveCallerSavedGroups(Settings.PreserveCallerSavedGroups),
      InitialRegYamlFile(Settings.InitialRegYamlFile) {

  initializeStackSection(Settings);
  initializeSelfcheckSection(Settings);
  initializeUtilitySection(Settings);
  initializeROMSection(Settings);
}

void SnippyProgramContext::createTargetContext(const Config &Cfg,
                                               const TargetSubtargetInfo &STI) {
  assert(!TargetContext && "Double context insertion");
  TargetContext =
      State->getSnippyTarget().createTargetContext(*State, Cfg, &STI);
}

SnippyProgramContext::~SnippyProgramContext() = default;

} // namespace snippy
} // namespace llvm
