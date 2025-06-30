#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Generator/Backtrack.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/SelfCheckInfo.h"
#include "snippy/Generator/SnippyModule.h"

#include "snippy/Simulator/SelfcheckObserver.h"
#include "../InitializePasses.h"

#define DEBUG_TYPE "snippy-simulator-context-wrapper"
#define PASS_DESC "Snippy Simulator Context Wrapper"

namespace llvm {

snippy::ActiveImmutablePassInterface *
createSimulatorContextWrapperPass(bool DoInit) {
  return new snippy::SimulatorContextWrapper(DoInit);
}

namespace snippy {

template class GenResultT<OwningSimulatorContext>;

void OwningSimulatorContext::initialize(SnippyProgramContext &ProgCtx,
                                        const TargetSubtargetInfo &SubTgt,
                                        const Config &Cfg,
                                        TargetGenContextInterface &TargetCtx,
                                        GlobalCodeFlowInfo &GCFI) {
  auto &Linker = ProgCtx.getLinker();
  auto &State = ProgCtx.getLLVMState();
  auto &SnippyTgt = State.getSnippyTarget();
  if (Cfg.hasTrackingMode())
    enableTrackingMode();
  OwnRunner = [&]() -> std::unique_ptr<SimRunner> {
    auto &ModelLibList = Cfg.PassCfg.ModelPluginConfig.ModelLibraries;
    if (ModelLibList.empty())
      return nullptr;

    auto Env = Interpreter::createSimulationEnvironment(ProgCtx, SubTgt, Cfg,
                                                        TargetCtx);

    return std::make_unique<SimRunner>(State.getCtx(), SnippyTgt, SubTgt,
                                       std::move(Env), ModelLibList);
  }();
  Runner = OwnRunner.get();

  if (!Runner) {
    if (hasTrackingMode())
      snippy::fatal(State.getCtx(), "Cannot generate snippet",
                    "requested selfcheck / backtrack / address-value-hazards, "
                    "but no model plugin provided.");
    return;
  }
  if (Cfg.PassCfg.InstrsGenerationConfig.ChainedRXSectionsFill &&
      std::count_if(
          Linker.sections().begin(), Linker.sections().end(),
          [](auto &Section) { return Section.OutputSection.Desc.M.X(); }) > 1 &&
      hasTrackingMode())
    snippy::fatal(State.getCtx(), "Cannot generate chained code routine",
                  "backtrack, selfcheck and address hazard mode do not work "
                  "with it yet.");
  if (Cfg.hasCallInstrs(ProgCtx.getOpcodeCache(), SnippyTgt) &&
      hasTrackingMode()) {
    snippy::fatal(State.getCtx(), "Cannot generate requested call instructions",
                  "backtrack and selfcheck do not work with calls yet.");
  }
  auto &TrackCfg = Cfg.getTrackCfg();
  OwnBT = TrackCfg.BTMode
              ? std::make_unique<Backtrack>(Runner->getPrimaryInterpreter())
              : nullptr;
  BT = OwnBT.get();

  OwnSCI =
      TrackCfg.SelfCheckPeriod ? std::make_unique<SelfCheckInfo>() : nullptr;
  SCI = OwnSCI.get();
}

static std::vector<NamedMemoryRange>
collectSectionsWithAccess(Interpreter &I, StringRef Selector) {
  auto AccessMask = AccMask{Selector};
  auto &Sects = I.getSections();
  std::vector<SectionDesc> SuitableSectDesc;
  std::copy_if(Sects.begin(), Sects.end(), std::back_inserter(SuitableSectDesc),
               [&AccessMask](const SectionDesc &SectDesc) {
                 return SectDesc.M == AccessMask;
               });
  auto Mapped = map_range(SuitableSectDesc, [](const SectionDesc &SectDesc) {
    return NamedMemoryRange{SectDesc.VMA, SectDesc.VMA + SectDesc.Size,
                            SectDesc.getIDString()};
  });
  return {Mapped.begin(), Mapped.end()};
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

static std::vector<NamedMemoryRange> collectRangesByExpr(Interpreter &I,
                                                         StringRef Selector) {
  auto ErrorRet = Selector.consume_front("{") && Selector.consume_back("}");
  assert(ErrorRet && "Wrong opt formating");
  auto RangeOpt = getRangeFromSelector(Selector);
  if (!RangeOpt) {
    return collectSectionsWithAccess(I, Selector);
  }
  return {*RangeOpt};
}

static bool isExprSelector(StringRef S) {
  return S.starts_with('{') && S.ends_with('}');
}

static std::vector<NamedMemoryRange> getRangesFromSelector(Interpreter &I,
                                                           StringRef Selector) {
  if (isExprSelector(Selector)) {
    return collectRangesByExpr(I, Selector);
  }
  auto RangeOpt = I.getSectionPosition(Selector);
  if (!RangeOpt)
    return {};
  return {*RangeOpt};
}

static std::vector<NamedMemoryRange>
getMemoryRangesToDump(Interpreter &I, ArrayRef<std::string> RangesSelectors) {
  std::vector<StringRef> Unknown;
  std::vector<NamedMemoryRange> RangesToDump;
  for (StringRef RangeSelector : RangesSelectors) {
    auto MemRanges = getRangesFromSelector(I, RangeSelector);
    copy(MemRanges, std::back_inserter(RangesToDump));
    if (MemRanges.empty() && !isExprSelector(RangeSelector))
      Unknown.push_back(RangeSelector);
  }
  if (!Unknown.empty()) {
    std::string Msg;
    raw_string_ostream OS(Msg);
    OS << "List of non-existent sections: ";
    interleaveComma(Unknown, OS, [&](StringRef S) { OS << '"' << S << '"'; });
    snippy::fatal("dump of unknown sections requested", Msg);
  }
  std::sort(RangesToDump.begin(), RangesToDump.end());
  RangesToDump.erase(std::unique(RangesToDump.begin(), RangesToDump.end()),
                     RangesToDump.end());
  return RangesToDump;
}

Error SimulatorContext::runSimulator(const RunInfo &RI) {
  auto &ImageToRun = RI.ImageToRun;
  auto &ProgCtx = RI.ProgCtx;
  auto &InitialStateOutputYaml = RI.InitialRegStateOutputYaml;
  auto &FinalStateOutputYaml = RI.FinalRegStateOutputYaml;
  auto &DumpMemorySection = RI.DumpMemorySection;
  auto &MemorySectionFile = RI.MemorySectionFile;

  // FIXME: unfortunately, it is not possible to implement interpretation as an
  // llvm pass (without creating a separate pass manager) due to peculiarities
  // of LLVM pipeline. The problem is that AsmPrinter does not actually
  // produce Elf file as a result of it's immediate execution.
  // Since it is a MachineFunction pass it creates only MC objects for each
  // function in Module. The actual object file generation happens during the
  // respected doFinalization call.  The problem with doFinalization is that it
  // runs after all passes have been run and the order of execution is not
  // Interpreters memory is set to the zero state after generation.
  auto &I = getInterpreter();
  auto &InitRegState = ProgCtx.getInitialRegisterState(I.getSubTarget());

  auto &SimRunner = getSimRunner();
  SimRunner.resetState(ProgCtx, RI.NeedMemoryReset);

  auto ElfData = ParsedElf::createParsedElf(
      ImageToRun, RI.EntryPointName, [](llvm::object::SectionRef Section) {
        if (auto EName = Section.getName())
          return EName->starts_with(".snippy");
        return false;
      });
  if (!ElfData)
    return ElfData.takeError();

  // FIXME: currently it does not initialize .bss sections with
  // zeroes, to comply with legacy behaviour.
  if (auto Err =
          SimRunner.loadElfSectionsToModel(*ElfData, /* InitBSS */ false))
    return Err;

  I.setRegisterState(InitRegState);
  // StartPC location may be updated since last time it was configured.

  I.setPC(ElfData->ProgStart);
  I.dumpCurrentRegState(InitialStateOutputYaml);

  if (ElfData->ProgEnd == std::nullopt)
    return createStringError(
        makeErrorCode(Errc::CorruptedElfImage),
        formatv("Elf does not have specified exit symbol"));

  auto ProgEnd = *ElfData->ProgEnd;
  SimRunner.run(ElfData->ProgStart, ProgEnd);

  I.dumpCurrentRegState(FinalStateOutputYaml);
  auto RangesToDump = getMemoryRangesToDump(I, DumpMemorySection);
  if (!RangesToDump.empty())
    I.dumpRanges(RangesToDump, std::string(MemorySectionFile));

  // Force flush stdout buffer written by Simulator.
  // It helps to avoid mixing it with stderr if redirected to same file.
  fflush(stdout);
  return Error::success();
}

char SimulatorContextWrapper::ID = 0;

StringRef SimulatorContextWrapper::getPassName() const {
  return PASS_DESC " Pass";
}

void SimulatorContextWrapper::getAnalysisUsage(AnalysisUsage &AU) const {
  if (DoInit) {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<FunctionGenerator>();
  }
  ModulePass::getAnalysisUsage(AU);
}

bool SimulatorContextWrapper::runOnModule(Module &M) {
  if (!DoInit)
    return false;
  auto &GC = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &FG = getAnalysis<FunctionGenerator>();
  auto &GCFI = FG.get<GlobalCodeFlowInfo>();
  auto &CGS = FG.getCallGraphState();

  auto &ProgCtx = GC.getProgramContext();
  const auto *EntryFun = CGS.getRootNode()->functions().front();
  auto &MainModule = SnippyModule::fromModule(M);
  const auto &SubTgt =
      MainModule.getMMI().getMachineFunction(*EntryFun)->getSubtarget();

  auto &TargetContext = ProgCtx.getTargetContext();
  auto &SimCtx = get<OwningSimulatorContext>();
  SimCtx.initialize(ProgCtx, SubTgt, GC.getConfig(), TargetContext, GCFI);

  return false;
}

OwningSimulatorContext::OwningSimulatorContext() = default;
OwningSimulatorContext::~OwningSimulatorContext() = default;

} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SimulatorContextWrapper;

SNIPPY_INITIALIZE_PASS(SimulatorContextWrapper, DEBUG_TYPE, PASS_DESC, false)
#undef DEBUG_TYPE
#undef PASS_DESC
#define DEBUG_TYPE "snippy-simulator-context-preserver"
#define PASS_DESC "Snippy Simulator Context Preserver"

namespace llvm {
ModulePass *createSimulatorContextPreserverPass() {
  return new snippy::SimulatorContextPreserver();
}

namespace snippy {

char SimulatorContextPreserver::ID = 0;

StringRef SimulatorContextPreserver::getPassName() const {
  return PASS_DESC " Pass";
}

void SimulatorContextPreserver::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<SimulatorContextWrapper>();
  ModulePass::getAnalysisUsage(AU);
}

bool SimulatorContextPreserver::runOnModule(Module &M) {
  auto &SimCtx =
      getAnalysis<SimulatorContextWrapper>().get<OwningSimulatorContext>();
  auto &MainModule = SnippyModule::fromModule(M);
  MainModule.getOrAddResult<OwningSimulatorContext>() = std::move(SimCtx);
  return false;
}

} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::SimulatorContextPreserver;

INITIALIZE_PASS(SimulatorContextPreserver, DEBUG_TYPE, PASS_DESC, false, false)
