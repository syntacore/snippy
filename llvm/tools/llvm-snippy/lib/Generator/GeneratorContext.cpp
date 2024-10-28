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
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/MemoryManager.h"
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

  auto &State = getProgramContext().getLLVMState();
  InitialMachineState = State.getSnippyTarget().createRegisterState(ST);

  if (!getInitialRegYamlFile().empty()) {
    WarningsT YamlWarnings;
    InitialMachineState->loadFromYamlFile(getInitialRegYamlFile(), YamlWarnings,
                                          &State.getSnippyTarget());
    std::for_each(YamlWarnings.begin(), YamlWarnings.end(), [&](StringRef Msg) {
      warn(WarningName::RegState, State.getCtx(), "register state yaml", Msg);
    });
    return *InitialMachineState;
  }

  InitialMachineState->randomize();
  return *InitialMachineState;
}

void GeneratorContext::initRunner() const {
  auto &State = getProgramContext().getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &Module = MainModule.getModule();
  const auto &EntryFun =
      *Module.getFunction(GenSettings->LinkerConfig.EntryPointName);
  const auto &SubTgt =
      MainModule.getMMI().getMachineFunction(EntryFun)->getSubtarget();

  auto Env = Interpreter::createSimulationEnvironment(*this);

  assert(hasModel() && "Model list must not be empty here");
  Runner = std::make_unique<SimRunner>(
      State.getCtx(), SnippyTgt, SubTgt, std::move(Env),
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
  Runner->getSimConfig().StartPC = getLinker().getStartPC();
  return *Runner;
}

Interpreter &GeneratorContext::getOrCreateInterpreter() const {
  return getOrCreateSimRunner().getPrimaryInterpreter();
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

template <typename InsertIt>
static void collectSectionsWithAccess(Interpreter &I, InsertIt Inserter,
                                      StringRef Selector) {
  auto AccessMask = AccMask{Selector};
  auto &Sects = I.getSections();
  std::vector<SectionDesc> SuitableSectDesc;
  std::copy_if(Sects.begin(), Sects.end(), std::back_inserter(SuitableSectDesc),
               [&AccessMask](const SectionDesc &SectDesc) {
                 return SectDesc.M == AccessMask;
               });
  std::transform(SuitableSectDesc.begin(), SuitableSectDesc.end(), Inserter,
                 [](const SectionDesc &SectDesc) {
                   return NamedMemoryRange{SectDesc.VMA,
                                           SectDesc.VMA + SectDesc.Size,
                                           SectDesc.getIDString()};
                 });
}

static void reportParsingError(Twine Msg) {
  snippy::fatal(formatv("Memory dump option parsing: {0}", Msg));
}

static size_t getAddressFromString(StringRef AddrString) {
  APInt Addr;
  constexpr auto Radix = 16u;

  if (AddrString.consumeInteger(Radix, Addr))
    reportParsingError("can't convert address to the integer: " +
                       Twine(AddrString));

  return Addr.getLimitedValue();
}

static std::optional<NamedMemoryRange>
getRangeFromSelector(StringRef Selector) {
  // example: 0x10-0x30
  Regex MemRangeRegex{"0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)"};
  SmallVector<StringRef> MatchedGroups;

  std::string Error;
  if (!MemRangeRegex.match(Selector, &MatchedGroups, &Error))
    return std::nullopt;

  assert(MatchedGroups.size() == 3);
  NamedMemoryRange FinalRange(getAddressFromString(MatchedGroups[1]),
                              getAddressFromString(MatchedGroups[2]));
  if (!FinalRange.isValid())
    reportParsingError("invalid range: " + Twine(Selector));

  return FinalRange;
}

template <typename InsertIt>
static void collectRangesByExpr(Interpreter &I, InsertIt Inserter,
                                StringRef Selector) {
  auto ErrorRet = Selector.consume_front("{") && Selector.consume_back("}");
  assert(ErrorRet && "Wrong opt formating");
  auto RangeOpt = getRangeFromSelector(Selector);
  if (!RangeOpt) {
    collectSectionsWithAccess(I, Inserter, Selector);
    return;
  }
  Inserter = *RangeOpt;
}

template <typename InsertIt>
static void getRangesFromSelector(Interpreter &I, InsertIt Inserter,
                                  StringRef Selector) {
  if (Selector.front() == '{' && Selector.back() == '}') {
    collectRangesByExpr(I, Inserter, Selector);
    return;
  }
  auto RangeOpt = I.getSectionPosition(Selector);
  if (!RangeOpt)
    snippy::fatal(formatv("failed to find a section {{0}}", Selector));
  Inserter = *RangeOpt;
}

static std::vector<NamedMemoryRange>
getMemoryRangesToDump(Interpreter &I,
                      snippy::opt_list<std::string> &RangesSelectors) {
  std::vector<NamedMemoryRange> RangesToDump;
  for (const auto &RangeSelector : RangesSelectors)
    getRangesFromSelector(I, std::back_inserter(RangesToDump), RangeSelector);

  std::sort(RangesToDump.begin(), RangesToDump.end());
  RangesToDump.erase(std::unique(RangesToDump.begin(), RangesToDump.end()),
                     RangesToDump.end());
  return RangesToDump;
}

void GeneratorContext::checkMemStateAfterSelfcheck() const {
  auto &SelfcheckSection = getProgramContext().getSelfcheckSection();
  std::vector<char> Data(SelfcheckSection.Size);
  auto &I = getOrCreateInterpreter();
  I.readMem(SelfcheckSection.VMA, Data);

  auto ResultOffset = 0;
  auto ReferenceOffset = getProgramContext().getSCStride();
  auto ChunksNum = 2;

  size_t BlockSize = ChunksNum * getProgramContext().getSCStride();

  auto DataSize = Data.size() - Data.size() % BlockSize;
  for (size_t Offset = 0; Offset < DataSize; Offset += BlockSize) {
    auto It = Data.begin() + Offset;
    auto ResultIt = It + ResultOffset;
    auto ReferenceIt = It + ReferenceOffset;
    for (size_t ByteIdx = 0; ByteIdx < getProgramContext().getSCStride();
         ++ByteIdx) {
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

void GeneratorContext::runSimulator(StringRef ImageToRun) {
  // FIXME: unfortunately, it is not possible to implement interpretation as an
  // llvm pass (without creating a separate pass manager) due to peculiarities
  // of LLVM pipeline. The problem is that AsmPrinter does not actually
  // produce Elf file as a result of it's immediate execution.
  // Since it is a MachineFunction pass it creates only MC objects for each
  // function in Module. The actual object file generation happens during the
  // respected doFinalization call.  The problem with doFinalization is that it
  // runs after all passes have been run and the order of execution is not
  // Interpreters memory is set to the zero state after generation.
  assert(GenSettings->ModelPluginConfig.RunOnModel);
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

  SimRunner.run(ImageToRun, InitRegState);

  I.dumpCurrentRegState(GenSettings->RegistersConfig.FinalStateOutputYaml);
  auto RangesToDump = getMemoryRangesToDump(I, DumpMemorySection);
  if (!RangesToDump.empty())
    I.dumpRanges(RangesToDump, MemorySectionFile);

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
}

namespace {

template <typename AccessPred>
std::optional<MemAddr> findPlaceForNewSection(SectionsDescriptions &Sections,
                                              AccessPred CustomModePred,
                                              size_t SectionSize,
                                              size_t SectionAlignment = 1) {
  assert(!Sections.empty());
  auto SectionBegin =
      std::find_if(Sections.begin(), Sections.end(), CustomModePred);
  assert(SectionBegin != Sections.end());

  auto SectionEnd =
      std::find_if(Sections.rbegin(), Sections.rend(), CustomModePred).base();

  // Try to emplace before all sections.
  if (SectionBegin == Sections.begin()) {
    const auto &Desc = *SectionBegin;
    if (Desc.VMA >= SectionSize) {
      auto Unaligned = Desc.VMA - SectionSize;
      return alignDown(Unaligned, SectionAlignment);
    }
  }

  // Try to emplace after all sections.
  if (SectionEnd == Sections.end()) {
    const auto &Desc = *std::prev(SectionEnd);
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
        const auto &Sec1Desc = Sec1;
        const auto &Sec2Desc = Sec2;
        auto Sec1End = Sec1Desc.VMA + Sec1Desc.Size;
        assert(Sec2Desc.VMA >= Sec1End);
        auto EndAligned = alignTo(Sec1End, SectionAlignment);
        if (EndAligned > Sec2Desc.VMA)
          return false;
        return Sec2Desc.VMA - EndAligned >= SectionSize;
      });

  if (FoundPlace != InsertPosEnd) {
    const auto &Desc = *FoundPlace;
    return alignTo(Desc.VMA + Desc.Size, SectionAlignment);
  } else
    return {};
}

SectionDesc allocateRWSection(SectionsDescriptions &Sections,
                              size_t SectionSize, StringRef Name,
                              size_t Alignment = 1) {
  auto IsRW = [](const SectionDesc &SE) {
    auto M = SE.M;
    return M.R() && M.W() && !M.X();
  };
  auto SecVMA = findPlaceForNewSection(Sections, IsRW, SectionSize, Alignment);
  if (!SecVMA)
    snippy::fatal(
        formatv("Failed to emplace selfcheck section: Could not find {0}"
                " bytes of consecutive free space outside of "
                "sections specified in layout.",
                SectionSize));

  auto SecVMAValue = SecVMA.value();
  auto SCSection =
      SectionDesc{Name, SecVMAValue, SectionSize, SecVMAValue, "RW"};
  return SCSection;
}
} // namespace

bool GeneratorContext::hasCallInstrs() const {
  auto &State = getProgramContext().getLLVMState();
  auto &SnippyTgt = State.getSnippyTarget();
  return GenSettings->Cfg.Histogram.hasCallInstrs(
      getProgramContext().getOpcodeCache(), SnippyTgt);
}

void GeneratorContext::diagnoseSelfcheckSection(size_t MinSize) const {
  auto &State = getProgramContext().getLLVMState();
  auto &SelfcheckSection = getProgramContext().getSelfcheckSection();
  auto M = SelfcheckSection.M;
  if (!(M.R() && M.W() && !M.X()))
    snippy::fatal(State.getCtx(), "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section must be RW");
  auto &SelfcheckSectionSize = SelfcheckSection.Size;
  if (SelfcheckSectionSize < MinSize)
    snippy::fatal(
        State.getCtx(),
        "Cannot use \"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
            "\" section from layout",
        "it does not fit selfcheck data (" + Twine(MinSize) + " bytes)");
  if ((SelfcheckSectionSize !=
       alignTo(SelfcheckSectionSize, SnippyProgramContext::getPageSize())) ||
      SelfcheckSection.VMA !=
          alignTo(SelfcheckSection.VMA, SnippyProgramContext::getPageSize()))
    snippy::fatal(State.getCtx(),
                  "Cannot use \"" +
                      Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section from layout",
                  "it has unaligned memory settings");
}

template <typename R>
void reportUnusedRXSectionWarning(LLVMContext &Ctx, R &&Names) {
  std::string NameList;
  llvm::raw_string_ostream OS{NameList};
  for (auto &&Name : Names) {
    OS << "'" << Name << "' ";
  }

  snippy::warn(WarningName::UnusedSection, Ctx,
               "Following RX sections are unused during generation", NameList);
}

static void
checkForUnusedRXSections(const Linker::LinkedSections &Sections,
                         const Linker::OutputSection &DefaultCodeSection,
                         LLVMContext &Ctx) {
  auto UnusedRXSections =
      llvm::make_filter_range(Sections, [&DefaultCodeSection](auto &S) {
        return S.OutputSection.Desc.M.X() &&
               S.OutputSection.Desc.getIDString() !=
                   DefaultCodeSection.Desc.getIDString();
      });
  auto UnusedRXSectionNames = llvm::map_range(UnusedRXSections, [](auto &S) {
    return S.OutputSection.Desc.getIDString();
  });
  if (!UnusedRXSectionNames.empty())
    reportUnusedRXSectionWarning(Ctx, UnusedRXSectionNames);
}

static std::vector<Linker::SectionEntry>
configureLinkerSections(LLVMContext &Ctx, Linker &L,
                        const GeneratorSettings &Settings) {
  assert(L.sections().hasOutputSectionFor(Linker::kDefaultTextSectionName));
  auto DefaultCodeSection =
      L.sections().getOutputSectionFor(Linker::kDefaultTextSectionName);
  std::vector<Linker::SectionEntry> ExecutionPath;
  if (Settings.InstrsGenerationConfig.ChainedRXSectionsFill)
    for (auto &RXSection : llvm::make_filter_range(L.sections(), [](auto &S) {
           return S.OutputSection.Desc.M.X();
         })) {
      if (!L.sections().hasOutputSectionFor(RXSection.OutputSection.Name))
        L.sections().addInputSectionFor(RXSection.OutputSection.Desc,
                                        RXSection.OutputSection.Name);
      ExecutionPath.push_back(RXSection);
    }

  if (Settings.InstrsGenerationConfig.ChainedRXSectionsFill) {
    if (Settings.InstrsGenerationConfig.ChainedRXSorted) {
      std::sort(ExecutionPath.begin(), ExecutionPath.end(),
                [](auto &LHS, auto &RHS) {
                  return LHS.OutputSection.Desc.getIDString() <
                         RHS.OutputSection.Desc.getIDString();
                });
    } else
      std::shuffle(ExecutionPath.begin(), ExecutionPath.end(),
                   RandEngine::engine());
  } else {
    checkForUnusedRXSections(L.sections(), DefaultCodeSection, Ctx);
    ExecutionPath.push_back(Linker::SectionEntry{
        DefaultCodeSection, {{std::string(Linker::kDefaultTextSectionName)}}});
  }
  return ExecutionPath;
}

namespace {

size_t getMinimumSelfcheckSize(const GeneratorSettings &Settings) {
  assert(Settings.TrackingConfig.SelfCheckPeriod);

  size_t BlockSize = 2 * SnippyProgramContext::getSCStride();
  // Note: There are cases when we have some problems for accurate calculating
  // of selcheck section size.
  //       Consequently it can potentially cause overflow of selfcheck
  //       section, So it's better to provide selfcheck section in Layout
  //       explicitly
  return alignTo(Settings.InstrsGenerationConfig.NumInstrs.value_or(0) *
                     BlockSize / Settings.TrackingConfig.SelfCheckPeriod,
                 SnippyProgramContext::getPageSize());
}

void amendSelfcheckSection(LLVMState &State, const GeneratorSettings &Settings,
                           SectionsDescriptions &Amended) {
  if (!Settings.TrackingConfig.SelfCheckPeriod)
    return;

  auto AlignSize = getMinimumSelfcheckSize(Settings);
  if (Amended.hasSection(SectionsDescriptions::SelfcheckSectionName))
    return;
  snippy::warn(
      WarningName::InconsistentOptions, State.getCtx(),
      "Implicit selfcheck section use is discouraged",
      "please, provide \"selfcheck\" section description in layout file");
  Amended.push_back(allocateRWSection(
      Amended, AlignSize, SectionsDescriptions::SelfcheckSectionName));
  return;
}

void amendStackSection(LLVMState &State, const GeneratorSettings &Settings,
                       SectionsDescriptions &Amended) {

  auto &SnippyTgt = State.getSnippyTarget();
  auto SP = Settings.RegistersConfig.StackPointer;
  auto Align = SnippyTgt.getSpillAlignmentInBytes(SP, State);

  if (Amended.hasSection(SectionsDescriptions::StackSectionName))
    return;

  if (StackSize.getValue()) {
    // Manually place stack section when only stack size is provided.
    Amended.push_back(allocateRWSection(Amended, StackSize.getValue(),
                                        SectionsDescriptions::StackSectionName,
                                        Align));
  } else if (Settings.LinkerConfig.ExternalStack) {
    // Implicitly allocate section when external stack is enabled but no
    // specific stack section info provided.
    assert(SectionsDescriptions::ImplicitSectionSize % Align == 0 &&
           "Wrong estimated stack size alignment.");
    Amended.push_back(
        allocateRWSection(Amended, SectionsDescriptions::ImplicitSectionSize,
                          SectionsDescriptions::StackSectionName, Align));
  }
}

void amendUtilitySection(LLVMState &State, const GeneratorSettings &Settings,
                         SectionsDescriptions &Amended) {
  // Don't do anything if section is not needed.
  if (Settings.RegistersConfig.SpilledToMem.empty())
    return;

  if (Amended.hasSection(SectionsDescriptions::UtilitySectionName))
    return;

  if (!Amended.hasSection(SectionsDescriptions::StackSectionName))
    return;
  // Try to use up some of a stack space.

  auto &Ctx = State.getCtx();
  auto Size = 2 * Settings.RegistersConfig.SpilledToMem.size() *
              State.getSnippyTarget().getAddrRegLen(State.getTargetMachine()) /
              CHAR_BIT;
  auto &Stack = Amended.getSection(SectionsDescriptions::StackSectionName);
  auto UtilitySection = SectionDesc(SectionsDescriptions::UtilitySectionName);
  UtilitySection.VMA = std::exchange(Stack.VMA, Stack.VMA + Size);
  UtilitySection.LMA = std::exchange(Stack.LMA, Stack.LMA + Size);
  UtilitySection.Size = Size;
  if (Stack.Size <= Size) {
    auto &Ctx = State.getCtx();
    snippy::fatal(
        Ctx, "Stack section is too small",
        "stack cannot be used for internal purposes. Either provide \"" +
            Twine(SectionsDescriptions::UtilitySectionName) +
            "\" section or increase \"" +
            Twine(SectionsDescriptions::StackSectionName) +
            "\" section size. (stack size: " + Twine(Stack.Size) +
            ", required size: " + Twine(Size) + ")");
  }
  Stack.Size -= Size;
  UtilitySection.M = Stack.M;
  Amended.push_back(UtilitySection);
  snippy::notice(WarningName::NotAWarning, Ctx,
                 "No \"" + Twine(SectionsDescriptions::UtilitySectionName) +
                     "\" section was provided",
                 "Using part of \"" +
                     Twine(SectionsDescriptions::StackSectionName) +
                     "\" section for internal purposes instead");
}

} // namespace

SectionsDescriptions
GeneratorSettings::getCompleteSectionList(LLVMState &State) const {
  auto CompleteList = Cfg.Sections;
  amendSelfcheckSection(State, *this, CompleteList);
  amendStackSection(State, *this, CompleteList);
  amendUtilitySection(State, *this, CompleteList);
  return CompleteList;
}

GeneratorContext::GeneratorContext(SnippyProgramContext &ProgContext,
                                   GeneratorSettings &Settings)
    : ProgContext(ProgContext), MainModule(ProgContext.getLLVMState(), "main"),
      ExecutionPath(
          snippy::configureLinkerSections(ProgContext.getLLVMState().getCtx(),
                                          ProgContext.getLinker(), Settings)),
      GenSettings(&Settings),
      FloatOverwriteSamplers(
          [&]() -> std::optional<FloatSemanticsSamplerHolder> {
            if (const auto &FPUConfig = GenSettings->Cfg.FPUConfig;
                FPUConfig.has_value() && FPUConfig->Overwrite.has_value())
              return FloatSemanticsSamplerHolder(*FPUConfig->Overwrite);
            return std::nullopt;
          }()),
      ImmHistMap([&] {
        const auto &ImmHist = Settings.Cfg.ImmHistogram;
        if (ImmHist.holdsAlternative<ImmediateHistogramRegEx>())
          return OpcodeToImmHistSequenceMap(
              ImmHist.get<ImmediateHistogramRegEx>(), Settings.Cfg.Histogram,
              ProgContext.getOpcodeCache());
        return OpcodeToImmHistSequenceMap();
      }()) {
  auto &State = ProgContext.getLLVMState();
  auto &Ctx = State.getCtx();

  auto GlobalSectionVMA = 0ull;
  auto GlobalSectionSize = 0ull;
  if (ProgContext.hasROMSection()) {
    auto &ROMSection = ProgContext.getROMSection();
    GlobalSectionVMA = ROMSection.VMA;
    GlobalSectionSize = ROMSection.Size;
  }
  GP = std::make_unique<GlobalsPool>(
      State, MainModule.getModule(), GlobalSectionVMA, GlobalSectionSize,
      ProgContext.exportedNamesMangled()
          ? ("__snippy_" + Twine(ProgContext.getEntryPointName()) + "_").str()
          : "__snippy_");

  HasTrackingMode = Settings.TrackingConfig.BTMode ||
                    Settings.TrackingConfig.SelfCheckPeriod ||
                    Settings.TrackingConfig.AddressVH;

  if (hasTrackingMode() && Settings.ModelPluginConfig.ModelLibraries.empty())
    snippy::fatal(State.getCtx(), "Cannot generate snippet",
                  "requested selfcheck / backtrack / address-value-hazards, "
                  "but no model plugin provided.");

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
    if (getRegisterPool().isReserved(RA))
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "return address register is explicitly reserved.");
  }
  auto SP = getProgramContext().getStackPointer();
  if (std::any_of(GenSettings->RegistersConfig.SpilledToStack.begin(),
                  GenSettings->RegistersConfig.SpilledToStack.end(),
                  [SP](auto Reg) { return Reg == SP; }))
    snippy::fatal("Stack pointer cannot be spilled. Remove it from "
                  "spill register list.");

  if (!getProgramContext().hasStackSection()) {
    if (!GenSettings->RegistersConfig.SpilledToStack.empty())
      snippy::fatal(Ctx, "Cannot spill requested registers",
                    "no stack space allocated.");

    if (hasCallInstrs() && GenSettings->Cfg.CGLayout.MaxLayers > 1u)
      snippy::fatal(
          State.getCtx(), "Cannot generate requested call instructions",
          "layout allows calls with depth>=1 but stack space is not provided.");
  }

  if (getProgramContext().hasExternalStack()) {
    if (GenSettings->ModelPluginConfig.RunOnModel)
      snippy::fatal(Ctx, "Cannot run snippet on model",
                    "external stack was enabled.");
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
  }

  if (GenSettings->Cfg.Sections.hasSection(
          SectionsDescriptions::SelfcheckSectionName) &&
      Settings.TrackingConfig.SelfCheckPeriod)
    diagnoseSelfcheckSection(getMinimumSelfcheckSize(*GenSettings));
}

void GeneratorContext::attachTargetContext(
    std::unique_ptr<TargetGenContextInterface> TgtContext) {
  TargetContext = std::move(TgtContext);
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
