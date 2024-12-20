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
                                        const GeneratorSettings &Settings,
                                        TargetGenContextInterface &TargetCtx,
                                        GlobalCodeFlowInfo &GCFI) {
  auto &Linker = ProgCtx.getLinker();
  auto &State = ProgCtx.getLLVMState();
  auto &SnippyTgt = State.getSnippyTarget();
  auto &FPUConfig = Settings.Cfg.FPUConfig;
  setTrackOptions(Settings.TrackingConfig,
                  FPUConfig && FPUConfig->needsModel());

  OwnRunner = [&]() -> std::unique_ptr<SimRunner> {
    auto &ModelLibList = Settings.ModelPluginConfig.ModelLibraries;
    if (ModelLibList.empty())
      return nullptr;
    auto MemCfg = MemoryConfig::getMemoryConfig(Linker, GCFI);

    auto Env = Interpreter::createSimulationEnvironment(
        ProgCtx, SubTgt, Settings, MemCfg, TargetCtx);

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
  if (Settings.InstrsGenerationConfig.ChainedRXSectionsFill &&
      std::count_if(
          Linker.sections().begin(), Linker.sections().end(),
          [](auto &Section) { return Section.OutputSection.Desc.M.X(); }) > 1 &&
      hasTrackingMode())
    snippy::fatal(State.getCtx(), "Cannot generate chained code routine",
                  "backtrack, selfcheck and address hazard mode do not work "
                  "with it yet.");
  if (Settings.Cfg.Histogram.hasCallInstrs(ProgCtx.getOpcodeCache(),
                                           SnippyTgt) &&
      hasTrackingMode()) {
    snippy::fatal(State.getCtx(), "Cannot generate requested call instructions",
                  "backtrack and selfcheck do not work with calls yet.");
  }
  OwnBT = Settings.TrackingConfig.BTMode
              ? std::make_unique<Backtrack>(Runner->getPrimaryInterpreter())
              : nullptr;
  BT = OwnBT.get();

  OwnSCI = Settings.TrackingConfig.SelfCheckPeriod
               ? std::make_unique<SelfCheckInfo>()
               : nullptr;
  SCI = OwnSCI.get();
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

void SimulatorContext::runSimulator(const RunInfo &RI) {
  auto &ImageToRun = RI.ImageToRun;
  auto &ProgCtx = RI.ProgCtx;
  auto &MainModule = RI.MainModule;
  auto &InitialStateOutputYaml = RI.InitialRegStateOutputYaml;
  auto &FinalStateOutputYaml = RI.FinalRegStateOutputYaml;
  auto &SelfCheckMem = RI.SelfcheckCheckMem;
  auto &DumpMemorySection = RI.DumpMemorySection;
  auto &MemorySectionFile = RI.MemorySectionFile;
  auto &BaseFilename = RI.BaseFilename;

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
  auto &L = ProgCtx.getLinker();
  auto &InitRegState = ProgCtx.getInitialRegisterState(I.getSubTarget());
  I.setInitialState(InitRegState);
  // StartPC location may be updated since last time it was configured.
  auto StartPC = *L.getStartPC();
  I.setPC(StartPC);
  I.dumpCurrentRegState(InitialStateOutputYaml);

  std::unique_ptr<RVMCallbackHandler::ObserverHandle<SelfcheckObserver>>
      SelfcheckObserverHandle;
  if (TrackOpts.SelfCheckPeriod) {
    auto &Map = MainModule.getOrAddResult<SelfCheckMap>().Map;
    // TODO: merge all infos from all modules.
    SelfcheckObserverHandle =
        I.setObserver<SelfcheckObserver>(Map.begin(), Map.end(), I.getPC());
  }

  auto &SimRunner = getSimRunner();

  SimRunner.run(ImageToRun, InitRegState, StartPC);

  I.dumpCurrentRegState(FinalStateOutputYaml);
  auto RangesToDump = getMemoryRangesToDump(I, DumpMemorySection);
  if (!RangesToDump.empty())
    I.dumpRanges(RangesToDump, std::string(MemorySectionFile));

  // Force flush stdout buffer written by Simulator.
  // It helps to avoid mixing it with stderr if redirected to same file.
  fflush(stdout);
  if (TrackOpts.SelfCheckPeriod) {
    if (SelfCheckMem)
      checkMemStateAfterSelfcheck(ProgCtx);

    auto AnnotationFilename =
        addExtensionIfRequired(BaseFilename, ".selfcheck.yaml");
    I.getObserverByHandle(*SelfcheckObserverHandle)
        .dumpAsYaml(AnnotationFilename);
  }
}

void SimulatorContext::checkMemStateAfterSelfcheck(
    SnippyProgramContext &ProgCtx) const {
  auto &SelfcheckSection = ProgCtx.getSelfcheckSection();
  std::vector<char> Data(SelfcheckSection.Size);
  auto &I = getInterpreter();
  I.readMem(SelfcheckSection.VMA, Data);

  const auto SCStride = ProgCtx.getSCStride();
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
  SimCtx.initialize(ProgCtx, SubTgt, GC.getGenSettings(), TargetContext, GCFI);

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
