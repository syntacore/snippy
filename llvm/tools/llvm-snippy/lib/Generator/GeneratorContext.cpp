//===-- GeneratorContext.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Simulator/SelfcheckObserver.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/Support/CommandLine.h"

#include <set>
#include <vector>

#define DEBUG_TYPE "snippy-gen-context"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<bool> SelfCheckMem(
    "selfcheck-mem",
    cl::desc("check a memory state after execution in selfcheck mode"),
    cl::Hidden, cl::init(true));

static snippy::opt<size_t> StackSize("stack-size",
                                     cl::desc("size of snippy stack space"),
                                     cl::cat(Options), cl::init(0u));

static snippy::opt<bool> ForceEmitLinkerScript(
    "force-emit-linker-script",
    cl::desc("Snippy will emit linker script even if linker not being run "),
    cl::cat(Options), cl::init(false));

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

GeneratorContext::~GeneratorContext() {}

const IRegisterState &
GeneratorContext::getInitialRegisterState(const TargetSubtargetInfo &ST) const {
  if (InitialMachineState)
    return *InitialMachineState;

  assert(State);
  InitialMachineState = State->getSnippyTarget().createRegisterState(ST);

  if (!getInitialRegYamlFile().empty()) {
    WarningsT YamlWarnings;
    InitialMachineState->loadFromYamlFile(getInitialRegYamlFile(),
                                          YamlWarnings);
    std::for_each(YamlWarnings.begin(), YamlWarnings.end(), [&](StringRef Msg) {
      warn(WarningName::RegState, State->getCtx(), "register state yaml", Msg);
    });
    return *InitialMachineState;
  }

  InitialMachineState->randomize();
  return *InitialMachineState;
}

void GeneratorContext::initRunner() const {
  const auto &SnippyTgt = State->getSnippyTarget();
  const auto &Module = *MMI->getModule();
  const auto &EntryFun =
      *Module.getFunction(GenSettings->LinkerConfig.EntryPointName);
  const auto &SubTgt = MMI->getMachineFunction(EntryFun)->getSubtarget();

  auto Env = Interpreter::createSimulationEnvironment(
      SnippyTgt, SubTgt, *PLinker, GenSettings->Cfg.MS, getTargetContext(),
      hasTrackingMode());

  assert(hasModel() && "Model list must not be empty here");
  Runner = std::make_unique<SimRunner>(
      State->getCtx(), SnippyTgt, SubTgt, std::move(Env),
      GenSettings->ModelPluginConfig.ModelLibraries);
}

void GeneratorContext::notifyMemUpdate(uint64_t Addr,
                                       const APInt &Value) const {
  // notifyMemUpdate can be called when Interpreter has not been created, so
  // We call getOrCreateInterpreter() instead of using 'I' directly
  if (hasTrackingMode())
    getOrCreateInterpreter().writeMem(Addr, Value);
}

SimRunner &GeneratorContext::getOrCreateSimRunner() const {
  assert(hasModel() && "Cannot create SimRunner. Model wasn't provided");
  if (!Runner)
    initRunner();
  return *Runner;
}

Interpreter &GeneratorContext::getOrCreateInterpreter() const {
  return getOrCreateSimRunner().getPrimaryInterpreter();
}

GeneratorResult
GeneratorContext::generateELF(ObjectFilesList InputImages) const {
  GeneratorResult Result;

  Result.SnippetImage = PLinker->run(InputImages, /*Relocatable*/ true);
  Result.LinkerScript = PLinker->generateLinkerScript();

  return Result;
}

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

template <typename It, typename InsertIt>
static void collectSectionsWithAccess(It SectBeg, It SectEnd, InsertIt Inserter,
                                      StringRef Selector) {
  auto ErrorRet = Selector.consume_front("{") && Selector.consume_back("}");
  assert(ErrorRet && "Wrong opt formating");
  auto AccessMask = AccMask{Selector};
  std::vector<SectionDesc> SuitableSectDesc;
  std::copy_if(SectBeg, SectEnd, std::back_inserter(SuitableSectDesc),
               [&AccessMask](const SectionDesc &SectDesc) {
                 return SectDesc.M == AccessMask;
               });
  std::transform(
      SuitableSectDesc.begin(), SuitableSectDesc.end(), Inserter,
      [](const SectionDesc &SectDesc) { return SectDesc.getIDString(); });
}

template <typename It, typename InsertIt>
static void getSectionsFromSelector(It SectBeg, It SectEnd, InsertIt Inserter,
                                    StringRef Selector) {
  if (Selector.front() == '{' && Selector.back() == '}') {
    collectSectionsWithAccess(SectBeg, SectEnd, Inserter, Selector);
    return;
  }
  auto SectIt =
      std::find_if(SectBeg, SectEnd, [&Selector](const SectionDesc &SectDesc) {
        return SectDesc.getIDString() == Selector;
      });
  if (SectIt == SectEnd)
    report_fatal_error("failed to find a section {" + Twine(Selector) + "}",
                       false);
  Inserter = SectIt->getIDString();
}

template <typename It>
static std::vector<std::string>
getSectionNamesToDump(It SectBeg, It SectEnd,
                      snippy::opt_list<std::string> &SectionsToDump) {
  std::set<std::string> SectionsSet;
  for (const auto &SectSelector : SectionsToDump)
    getSectionsFromSelector(SectBeg, SectEnd,
                            std::inserter(SectionsSet, SectionsSet.begin()),
                            SectSelector);
  return {SectionsSet.begin(), SectionsSet.end()};
}

std::string
GeneratorContext::generateLinkedImage(ObjectFilesList InputImages) const {
  return PLinker->run(InputImages, /*Relocatable*/ false);
}

void GeneratorContext::checkMemStateAfterSelfcheck() const {
  std::vector<char> Data(SelfcheckSection.Size);
  auto &I = getOrCreateInterpreter();
  I.readMem(SelfcheckSection.VMA, Data);

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
        report_fatal_error("Incorrect memory state after interpretation in "
                           "self-check mode. Error is in block @ 0x" +
                               Twine::utohexstr(FaultAddr) + "{" +
                               Twine::utohexstr(SelfcheckSection.VMA) + " + " +
                               Twine::utohexstr(Offset) + " + " +
                               Twine::utohexstr(ByteIdx) + "}\n",
                           false);
      }
    }
  }
}

void GeneratorContext::runSimulator(StringRef ImageToRun) {
  // FIXME: unfortunately, it is not possible to implement interpretation as an
  // llvm pass (without creating a separate pass manager) due to peculiarities
  // of LLVM pipeline. The problem is that AsmPrinter does not actually
  // produce Elf file as a result of it's immediate execution.
  // Since it is a MachineFunction pass it creates only MC objects for each
  // function in Module. The actual object file generation happens during the
  // respected doFinalization call.  The problem with doFinalization is that it
  // runs after all passes have been run and the order of execution is not
  if (GenSettings->ModelPluginConfig.RunOnModel) {
    // Interpreters memory is set to the zero state after generation.
    auto &I = getOrCreateInterpreter();
    auto &InitRegState = getInitialRegisterState(I.getSubTarget());
    I.setInitialState(InitRegState);
    I.dumpCurrentRegState(GenSettings->RegistersConfig.InitialStateOutputYaml);

    std::unique_ptr<RVMCallbackHandler::ObserverHandle<SelfcheckObserver>>
        SelfcheckObserverHandle;
    if (GenSettings->TrackingConfig.SelfCheckPeriod)
      SelfcheckObserverHandle = I.setObserver<SelfcheckObserver>(
          SelfcheckMap.begin(), SelfcheckMap.end(), I.getPC());

    auto &SimRunner = getOrCreateSimRunner();

    auto FinalPC = SimRunner.run(ImageToRun, InitRegState);

    I.setPC(FinalPC);
    I.dumpCurrentRegState(GenSettings->RegistersConfig.FinalStateOutputYaml);
    auto InterpSections = I.getSections();
    auto SectNames = getSectionNamesToDump(
        InterpSections.begin(), InterpSections.end(), DumpMemorySection);
    if (!SectNames.empty())
      I.dumpSections(SectNames, MemorySectionFile);

    // Force flush stdout buffer written by Simulator.
    // It helps to avoid mixing it with stderr if redirected to same file.
    fflush(stdout);
    if (GenSettings->TrackingConfig.SelfCheckPeriod) {
      if (SelfCheckMem)
        checkMemStateAfterSelfcheck();

      auto AnnotationFilename =
          addExtensionIfRequired(GenSettings->BaseFileName, ".selfcheck.yaml");
      I.getObserverByHandle(*SelfcheckObserverHandle)
          .dumpAsYaml(AnnotationFilename);
    }
  } else {
    snippy::warn(WarningName::NoModelExec, State->getCtx(),
                 "Skipping snippet execution on the model",
                 "model was set no 'None'.");
  }
}

namespace {

template <typename AccessPred>
std::optional<MemAddr>
findPlaceForNewSection(const Linker &L, AccessPred CustomModePred,
                       size_t SectionSize, size_t SectionAlignment = 1) {
  const auto &Sections = L.sections();
  assert(!Sections.empty());
  auto SectionBegin =
      std::find_if(Sections.begin(), Sections.end(), CustomModePred);
  assert(SectionBegin != Sections.end());

  auto SectionEnd =
      std::find_if(Sections.rbegin(), Sections.rend(), CustomModePred).base();

  // Try to emplace before all sections.
  if (SectionBegin == Sections.begin()) {
    const auto &Desc = SectionBegin->OutputSection.Desc;
    if (Desc.VMA >= SectionSize) {
      auto Unaligned = Desc.VMA - SectionSize;
      return alignDown(Unaligned, SectionAlignment);
    }
  }

  // Try to emplace after all sections.
  if (SectionEnd == Sections.end()) {
    const auto &Desc = std::prev(SectionEnd)->OutputSection.Desc;
    auto Unaligned = Desc.VMA + Desc.Size;
    auto Aligned = alignTo(Unaligned, SectionAlignment);
    if (std::numeric_limits<size_t>::max() - Aligned >= SectionSize) {
      return Aligned;
    }
  }

  // Finally, try to find place among sections.
  auto InsertPosBegin =
      SectionBegin == Sections.begin() ? SectionBegin : std::prev(SectionBegin);
  auto InsertPosEnd =
      SectionEnd == Sections.end() ? SectionEnd : std::next(SectionEnd);

  auto FoundPlace = std::adjacent_find(
      InsertPosBegin, InsertPosEnd,
      [SectionSize, SectionAlignment](const auto &Sec1, const auto &Sec2) {
        const auto &Sec1Desc = Sec1.OutputSection.Desc;
        const auto &Sec2Desc = Sec2.OutputSection.Desc;
        auto Sec1End = Sec1Desc.VMA + Sec1Desc.Size;
        assert(Sec2Desc.VMA >= Sec1End);
        auto EndAligned = alignTo(Sec1End, SectionAlignment);
        if (EndAligned > Sec2Desc.VMA)
          return false;
        return Sec2Desc.VMA - EndAligned >= SectionSize;
      });

  if (FoundPlace != InsertPosEnd) {
    const auto &Desc = FoundPlace->OutputSection.Desc;
    return alignTo(Desc.VMA + Desc.Size, SectionAlignment);
  } else
    return {};
}

SectionDesc allocateRWSection(Linker &L, size_t SectionSize, StringRef Name,
                              size_t Alignment = 1) {
  auto IsRW = [](const Linker::SectionEntry &SE) {
    auto M = SE.OutputSection.Desc.M;
    return M.R() && M.W() && !M.X();
  };
  auto SecVMA = findPlaceForNewSection(L, IsRW, SectionSize, Alignment);
  if (!SecVMA)
    report_fatal_error("Failed to emplace selfcheck section: Could not find " +
                           Twine(SectionSize) +
                           " bytes of consecutive free space outside of "
                           "sections specified in layout.",
                       false);

  auto SecVMAValue = SecVMA.value();
  auto SCSection =
      SectionDesc{Name, SecVMAValue, SectionSize, SecVMAValue, "RW"};
  return SCSection;
}
} // namespace

bool GeneratorContext::hasCallInstrs() const {
  auto &SnippyTgt = State->getSnippyTarget();
  return GenSettings->Cfg.Histogram.hasCallInstrs(*OpCC, SnippyTgt);
}

void GeneratorContext::initializeStackSection() {
  auto &Ctx = State->getCtx();
  auto &SnippyTgt = State->getSnippyTarget();
  auto SP = SnippyTgt.getStackPointer();
  auto Align = SnippyTgt.getSpillAlignmentInBytes(SP, *this);

  if (GenSettings->LinkerConfig.ExternalStack) {
    if (GenSettings->ModelPluginConfig.RunOnModel)
      snippy::fatal(Ctx, "Cannot run snippet on model",
                    "external stack was enabled.");
    if (RegPoolsStorage.front().isReserved(SP))
      snippy::fatal(Ctx, "Cannot configure external stack",
                    "stack pointer register is "
                    "explicitly reserved.");

    if (GenSettings->Cfg.Sections.hasSection(
            SectionsDescriptions::StackSectionName)) {
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "Section 'stack' will not be used",
                   "external stack was enabled.");
    }

    if (StackSize.getValue())
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "--" + Twine(StackSize.ArgStr) + " option ignored",
                   "external stack was enabled.");
    return;
  }

  if (GenSettings->Cfg.Sections.hasSection(
          SectionsDescriptions::StackSectionName)) {
    // Configure stack from layout.
    StackSection = GenSettings->Cfg.Sections.getSection(
        SectionsDescriptions::StackSectionName);
    auto M = StackSection.value().M;
    if (!(M.R() && M.W() && !M.X()))
      snippy::fatal(Ctx, "Wrong layout file",
                    "\"" + Twine(SectionsDescriptions::StackSectionName) +
                        "\" section must be RW");
    if (StackSize.getValue())
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "--" + Twine(StackSize.ArgStr) + " option ignored",
                   "found stack section in layout");

  } else if (StackSize.getValue()) {
    // Manually place stack section when only stack size is provided.
    StackSection =
        allocateRWSection(*PLinker, StackSize.getValue(),
                          SectionsDescriptions::StackSectionName, Align);
    PLinker->addSection(StackSection.value());
  } else if (GenSettings->LinkerConfig.ExternalStack) {
    // Implicitly allocate section when external stack is enabled but no
    // specific stack section info provided.
    assert(SectionsDescriptions::ImplicitSectionSize % Align == 0 &&
           "Wrong estimated stack size alignment.");
    StackSection =
        allocateRWSection(*PLinker, SectionsDescriptions::ImplicitSectionSize,
                          SectionsDescriptions::StackSectionName, Align);
    PLinker->addSection(StackSection.value());
  }

  if (StackSection.has_value()) {
    if (StackSection.value().VMA % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section VMA must be " + Twine(Align) +
                        " bytes aligned.");
    if (StackSection.value().Size % Align != 0)
      snippy::fatal(Ctx, "Stack configure failed",
                    "Stack section size must be " + Twine(Align) +
                        " bytes aligned.");

    if (RegPoolsStorage.front().isReserved(SP))
      snippy::fatal(Ctx, "Failed to initialize stack",
                    "stack pointer register is "
                    "explicitly reserved.");

    if (std::any_of(GenSettings->RegistersConfig.SpilledRegs.begin(),
                    GenSettings->RegistersConfig.SpilledRegs.end(),
                    [SP](auto Reg) { return Reg == SP; }))
      report_fatal_error("Stack pointer cannot be spilled. Remove it from "
                         "spill register list.",
                         false);
  } else {
    if (!GenSettings->RegistersConfig.SpilledRegs.empty())
      snippy::fatal(Ctx, "Cannot spill requested registers",
                    "no stack space allocated.");

    if (hasCallInstrs() && GenSettings->Cfg.CGLayout.MaxLayers > 1u)
      snippy::fatal(
          State->getCtx(), "Cannot generate requested call instructions",
          "layout allows calls with depth>=1 but stack space is not provided.");
  }
}

void GeneratorContext::diagnoseSelfcheckSection(size_t MinSize) const {
  auto M = SelfcheckSection.M;
  if (!(M.R() && M.W() && !M.X()))
    snippy::fatal(State->getCtx(), "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section must be RW");
  auto &SelfcheckSectionSize = SelfcheckSection.Size;
  if (SelfcheckSectionSize < MinSize)
    snippy::fatal(
        State->getCtx(),
        "Cannot use \"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
            "\" section from layout",
        "it does not fit selfcheck data (" + Twine(MinSize) + " bytes)");
  if ((SelfcheckSectionSize != alignTo(SelfcheckSectionSize, kPageSize)) ||
      SelfcheckSection.VMA != alignTo(SelfcheckSection.VMA, kPageSize))
    snippy::fatal(State->getCtx(),
                  "Cannot use \"" +
                      Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section from layout",
                  "it has unaligned memory settings");
}

GeneratorContext::GeneratorContext(MachineModuleInfo &MMI, Module &M,
                                   LLVMState &State, RegPool &Pool,
                                   RegisterGenerator &RegGen,
                                   GeneratorSettings &Settings,
                                   const OpcodeCache &OpCc)
    : MMI(&MMI), State(&State), RegPoolsStorage({Pool}), RegGen(&RegGen),
      CGS(std::make_unique<CallGraphState>()), OpCC(&OpCc),
      PLinker(std::make_unique<Linker>(
          State.getCtx(), Settings.Cfg.Sections,
          Settings.InstrsGenerationConfig.ChainedRXSectionsFill,
          Settings.InstrsGenerationConfig.ChainedRXSorted,
          Settings.LinkerConfig.MangleExportedNames
              ? Settings.LinkerConfig.EntryPointName
              : "")),
      GenSettings(&Settings), ImmHistMap([&Settings, &OpCc] {
        const auto &ImmHist = Settings.Cfg.ImmHistogram;
        if (ImmHist.holdsAlternative<ImmediateHistogramRegEx>())
          return OpcodeToImmHistSequenceMap(
              ImmHist.get<ImmediateHistogramRegEx>(), Settings.Cfg.Histogram,
              OpCc);
        return OpcodeToImmHistSequenceMap();
      }()) {
  auto SnippyDataVMA = 0ull;
  auto SnippyDataSize = 0ull;

  HasTrackingMode = Settings.TrackingConfig.BTMode ||
                    Settings.TrackingConfig.SelfCheckPeriod ||
                    Settings.TrackingConfig.AddressVH;

  if (hasTrackingMode() && Settings.ModelPluginConfig.ModelLibraries.empty())
    snippy::fatal(State.getCtx(), "Cannot generate snippet",
                  "requested selfcheck / backtrack / address-value-hazards, "
                  "but no model plugin provided.");
  if (PLinker->hasOutputSectionFor(".rodata")) {
    auto ROMSection = PLinker->getOutputSectionFor(".rodata");
    SnippyDataVMA = ROMSection.Desc.VMA;
    SnippyDataSize = ROMSection.Desc.Size;
  }
  GP = std::make_unique<GlobalsPool>(
      State, M, SnippyDataVMA, SnippyDataSize,
      Settings.LinkerConfig.MangleExportedNames
          ? ("__snippy_" + Twine(Settings.LinkerConfig.EntryPointName) + "_")
                .str()
          : "__snippy_");

  if (Settings.TrackingConfig.SelfCheckPeriod) {
    size_t BlockSize = 2 * SCStride;
    // Note: There are cases when we have some problems for accurate calculating
    // of selcheck section size.
    //       Consequently it can potentially cause overflow of selfcheck
    //       section, So it's better to provide selfcheck section in Layout
    //       explicitly
    auto AlignSize =
        alignTo(getRequestedInstrsNumForMainFunction() * BlockSize /
                    Settings.TrackingConfig.SelfCheckPeriod,
                kPageSize);
    if (GenSettings->Cfg.Sections.hasSection(
            SectionsDescriptions::SelfcheckSectionName)) {
      // Configure selfcheck from layout.
      SelfcheckSection = GenSettings->Cfg.Sections.getSection(
          SectionsDescriptions::SelfcheckSectionName);
      diagnoseSelfcheckSection(AlignSize);
    } else {
      snippy::warn(
          WarningName::InconsistentOptions, State.getCtx(),
          "Implicit selfcheck section use is discouraged",
          "please, provide \"selfcheck\" section description in layout file");
      SelfcheckSection = allocateRWSection(
          *PLinker, AlignSize, SectionsDescriptions::SelfcheckSectionName);
      PLinker->addSection(SelfcheckSection);
    }
  }

  initializeStackSection();

  if (Settings.InstrsGenerationConfig.ChainedRXSectionsFill &&
      std::count_if(
          getLinker().sections().begin(), getLinker().sections().end(),
          [](auto &Section) { return Section.OutputSection.Desc.M.X(); }) > 1 &&
      hasTrackingMode())
    snippy::fatal(State.getCtx(), "Cannot generate chained code routine",
                  "backtrack, selfcheck and address hazard mode do not work "
                  "with it yet.");
  if (hasCallInstrs()) {
    const auto &RI = State.getRegInfo();
    auto RA = RI.getRARegister();
    if (hasTrackingMode())
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "backtrack and selfcheck do not work with calls yet.");
    if (RegPoolsStorage.front().isReserved(RA))
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "return address register is explicitly reserved.");
  }
}

void GeneratorContext::attachTargetContext(
    std::unique_ptr<TargetGenContextInterface> TgtContext) {
  TargetContext = std::move(TgtContext);
}
GenPolicy GeneratorContext::createGenerationPolicy(
    unsigned Limit, OpcodeFilter Filter, bool MustHavePrimaryInstrs,
    std::optional<unsigned> BurstGroupID,
    ArrayRef<OpcodeHistogramEntry> Overrides) const {
  if (BurstGroupID.has_value())
    return std::make_unique<BurstGenPolicy>(*this, BurstGroupID.value());
  return std::make_unique<DefaultGenPolicy>(*this, Filter,
                                            MustHavePrimaryInstrs, Overrides);
}

void GeneratorContext::addIncomingValues(const MachineBasicBlock *MBB,
                                         RegToValueType RegToValue) {
  assert(MBB);
  assert(IncomingValues.count(MBB) == 0);
  IncomingValues[MBB] = std::move(RegToValue);
}

const GeneratorContext::RegToValueType &
GeneratorContext::getIncomingValues(const MachineBasicBlock *MBB) {
  assert(MBB);
  assert(IncomingValues.count(MBB));
  return IncomingValues[MBB];
}

} // namespace snippy
} // namespace llvm
