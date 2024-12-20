//===-- FunctionGeneratorPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../InitializePasses.h"

#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/YAMLTraits.h"

#include <stack>

#define DEBUG_TYPE "snippy-function-generator"
#define PASS_DESC "Snippy Function Generator"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

snippy::opt<unsigned> GraphDensity("call-graph-density",
                                   cl::desc("number of iterations in loops"),
                                   cl::cat(Options), cl::init(5), cl::Hidden);

snippy::opt<bool>
    ForceConnect("call-graph-force-connect",
                 cl::desc("generate additional edges in call graph "
                          "to make all nodes reachable from root"),
                 cl::cat(Options), cl::init(false), cl::Hidden);

static snippy::opt<std::string>
    DumpCGFilename("call-graph-dump-filename",
                   cl::desc("Specify file to dump call graph in dot format"),
                   cl::value_desc("filename"), cl::init(""), cl::cat(Options));

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

void FunctionGenerator::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GeneratorContextWrapper>();
  ModulePass::getAnalysisUsage(AU);
}
StringRef FunctionGenerator::getPassName() const { return PASS_DESC " Pass"; }

char FunctionGenerator::ID = 0;

namespace {

struct LayerMapEntry {
  CallGraphState::Node *Node;
  int Layer;
  LayerMapEntry(CallGraphState::Node *Node, int Layer)
      : Node(Node), Layer(Layer){};
};

void fillReachableNodes(CallGraphState::Node *Root,
                        DenseSet<CallGraphState::Node *> &Reachables) {
  std::stack<CallGraphState::Node *> TraverseStack;

  TraverseStack.push(Root);
  while (!TraverseStack.empty()) {
    auto CurNode = TraverseStack.top();
    TraverseStack.pop();
    Reachables.insert(CurNode);
    for (auto &Callee : CurNode->callees())
      if (!Reachables.contains(Callee))
        TraverseStack.push(Callee);
  }
}

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::FunctionGenerator;

SNIPPY_INITIALIZE_PASS(FunctionGenerator, DEBUG_TYPE, PASS_DESC, false)

namespace llvm {

snippy::ActiveImmutablePassInterface *createFunctionGeneratorPass() {
  return new FunctionGenerator();
}

} // namespace llvm

namespace llvm {

void yaml::MappingTraits<snippy::FunctionDesc>::mapping(
    yaml::IO &IO, snippy::FunctionDesc &Desc) {
  IO.mapOptional("name", Desc.Name);
  IO.mapOptional("external", Desc.External);
  IO.mapOptional("callees", Desc.Callees);
}

std::string yaml::MappingTraits<snippy::FunctionDesc>::validate(
    yaml::IO &IO, snippy::FunctionDesc &Info) {
  if (Info.Name.empty())
    return "Empty names are not allowed for function in call graph";
  if (Info.External && !Info.Callees.empty())
    return "Function specified as external cannot have callee functions";
  return "";
}

void yaml::MappingTraits<snippy::FunctionDescs>::mapping(
    yaml::IO &IO, snippy::FunctionDescs &Desc) {
  IO.mapOptional("function-list", Desc.Descs);
  IO.mapOptional("entry-point", Desc.EntryPoint);
}

std::string yaml::MappingTraits<snippy::FunctionDescs>::validate(
    yaml::IO &IO, snippy::FunctionDescs &Info) {
  if (Info.Descs.empty())
    return "Read empty call graph";
  auto EPIt = Info.getEntryPointDesc();
  if (EPIt == Info.Descs.end())
    return "No description found for specified entry point '" +
           Info.EntryPoint + "' in call graph file";
  auto &EntryPoint = *EPIt;
  if (EntryPoint.External)
    return "Specified entry point '" + EntryPoint.Name +
           "' must not be marked as external in call graph file";
  std::unordered_set<std::string> Names;
  for (auto &Desc : Info.Descs) {
    if (Names.count(Desc.Name))
      return "Call graph has multiple entries for '" + Desc.Name + "' function";
    Names.emplace(Desc.Name);
  }
  for (auto &Desc : Info.Descs)
    for (auto &Callee : Desc.Callees) {
      if (!Names.count(Callee))
        return "Function '" + Desc.Name + "' has '" + Callee +
               "' in the callee list, that has no description in yaml";
    }
  return "";
}

namespace snippy {
MachineFunction &FunctionGenerator::createFunction(
    GeneratorContext &SGCtx, Module &M, StringRef Name, StringRef SectionName,
    Function::LinkageTypes Linkage, size_t NumInstr) {

  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  std::string FinalName =
      SectionName.empty() || Linkage != Function::InternalLinkage
          ? std::string(Name)
          : (Twine(SectionName) + "." + Name).str();
  auto &MF = State.createMachineFunctionFor(
      State.createFunction(M, FinalName, SectionName, Linkage),
      SnippyModule::fromModule(M).getMMI());
  auto &Props = MF.getProperties();
  auto &SnippyTgt = State.getSnippyTarget();
  auto &GenSettings = SGCtx.getGenSettings();
  auto &OpCC = ProgCtx.getOpcodeCache();
  // FIXME: currently we don't keep liveness when creating and filling new BB
  auto IsRegsInit = GenSettings.RegistersConfig.InitializeRegs;
  if (GenSettings.hasCFInstrs(OpCC) ||
      GenSettings.hasCallInstrs(OpCC, SnippyTgt) || IsRegsInit)
    Props.reset(MachineFunctionProperties::Property::TracksLiveness);
  auto *MBB = createMachineBasicBlock(MF);
  assert(MBB);
  MF.push_back(MBB);
  setRequestedInstrNum(MF, NumInstr);

  return MF;
}

// Get list of RX sections. Root functions must be placed to
// that sections in order. That is, entry function is assigned
// first section in list, exit function is assigned last section
// in list and all intermediate root functions goes between them
// in order.
std::vector<std::string> FunctionGenerator::prepareRXSections() {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &GCFI = get<GlobalCodeFlowInfo>();
  if (!SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXSectionsFill)
    return {""};

  std::vector<std::string> Ret;
  for (auto &&[_, InputSections] : GCFI.ExecutionPath)
    Ret.emplace_back(InputSections.front().Name);

  return Ret;
}

void FunctionGenerator::initRootFunctions(Module &M, StringRef EntryPointName) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &CGS = get<GlobalCodeFlowInfo>().CGS;
  auto RFs = distributeRootFunctions();
  auto &&[EntryFnSection, EntryFnInstrNum] = RFs.front();
  // Entry point produces multiple root functions. Each one of
  // them is assigned to respective RX section.
  auto &MF = createFunction(SGCtx, M, EntryPointName, EntryFnSection,
                            Function::ExternalLinkage, EntryFnInstrNum);
  auto *N = CGS.emplaceNode(&(MF.getFunction()));
  CGS.setRoot(N);
  std::vector<Function *> RestRootFs;

  std::transform(std::next(RFs.begin()), RFs.end(),
                 std::back_inserter(RestRootFs), [&](auto &S) {
                   return &createFunction(SGCtx, M, EntryPointName,
                                          S.SectionName,
                                          Function::InternalLinkage, S.InstrNum)
                               .getFunction();
                 });
  if (!SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXSorted)
    RandEngine::shuffle(RestRootFs.begin(), RestRootFs.end());
  for (auto *F : RestRootFs)
    CGS.appendNode(N, F);
}

std::vector<FunctionGenerator::RootFnPlacement>
FunctionGenerator::distributeRootFunctions() {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &Linker = ProgCtx.getLinker();
  auto &GenSettings = SGCtx.getGenSettings();
  if (!GenSettings.InstrsGenerationConfig.ChainedRXSectionsFill)
    return {RootFnPlacement(
        std::string{""}, GenSettings.getRequestedInstrsNumForMainFunction())};

  auto Sections = prepareRXSections();

  std::vector<RootFnPlacement> Ret;
  auto GetInstrNum = [&SGCtx, &GenSettings](auto SectionSize) {
    return static_cast<size_t>(std::llround(
        (double)GenSettings.getRequestedInstrsNumForMainFunction() *
        ((double)SectionSize /
         (double)SGCtx.getGenSettings().Cfg.Sections.getSectionsSize(Acc::X))));
  };

  std::transform(
      Sections.begin(), Sections.end(), std::back_inserter(Ret), [&](auto &S) {
        assert(Linker.sections().hasOutputSectionFor(S));
        auto SectionSize = Linker.sections().getOutputSectionFor(S).Desc.Size;
        return RootFnPlacement{S, GetInstrNum(SectionSize)};
      });

  if (!SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXChunkSize)
    return Ret;

  auto ChunkSize =
      *SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXChunkSize;

  decltype(Ret) RetSplit;

  // Split part for each section into pieces of size ChunkSize.
  for (auto &&[Name, NumInstr] : Ret) {
    auto FunCount = NumInstr / ChunkSize + 1u;
    auto LastFunIC = NumInstr % ChunkSize;
    // All except last function have exactly ChunkSize instructions.
    std::fill_n(std::back_inserter(RetSplit), FunCount - 1u,
                RootFnPlacement(Name, ChunkSize));
    // Last function has remaining number of instructions(not greater than
    // ChunkSize).
    RetSplit.emplace_back(Name, LastFunIC);
  }
  return RetSplit;
}
template <typename R>
static void reportUnusedRXSectionWarning(LLVMContext &Ctx, R &&Names) {
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

void FunctionGenerator::initExecutionPath() {
  auto &ExecutionPath = get<GlobalCodeFlowInfo>().ExecutionPath;
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &Settings = SGCtx.getGenSettings();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &L = SGCtx.getProgramContext().getLinker();
  assert(L.sections().hasOutputSectionFor(Linker::kDefaultTextSectionName));
  auto DefaultCodeSection =
      L.sections().getOutputSectionFor(Linker::kDefaultTextSectionName);
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
      RandEngine::shuffle(ExecutionPath.begin(), ExecutionPath.end());
  } else {
    checkForUnusedRXSections(L.sections(), DefaultCodeSection, State.getCtx());
    ExecutionPath.push_back(Linker::SectionEntry{
        DefaultCodeSection, {{std::string(Linker::kDefaultTextSectionName)}}});
  }
  // Setup StartPC for later initialize model with.
  auto &FirstSection = ExecutionPath.front();
  auto StartPC = FirstSection.OutputSection.Desc.VMA;
  L.setStartPC(StartPC);
}

bool FunctionGenerator::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &Ctx = State.getCtx();
  auto &LLVMTM = State.getTargetMachine();
  StringRef ABIName = SGCtx.getGenSettings().getABIName();
  initExecutionPath();
  if (ABIName.size()) {
    auto *ABINameMD = MDString::get(Ctx, ABIName);
    M.setModuleFlag(Module::ModFlagBehavior::Error, "target-abi", ABINameMD);
  }

  M.setDataLayout(LLVMTM.createDataLayout());

  auto Ret = !SGCtx.getConfig().FuncDescs.has_value()
                 ? generateDefault(M)
                 : readFromYaml(M, *SGCtx.getConfig().FuncDescs);
  auto CGFilename = DumpCGFilename.getValue();
  if (!CGFilename.empty())
    getCallGraphState().dump(CGFilename, CGDumpFormat);

  return Ret;
}

bool FunctionGenerator::readFromYaml(Module &M, const FunctionDescs &FDs) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &CGS = get<GlobalCodeFlowInfo>().CGS;
  auto &Descs = FDs.Descs;

  auto EPIt = FDs.getEntryPointDesc();
  assert(EPIt != FDs.Descs.end() && "that should be checked earlier");
  auto &EntryPoint = *EPIt;

  auto Sections = prepareRXSections();

  std::map<std::string, CallGraphState::Node *> NameMap;

  // Create functions.
  for (auto &Desc : Descs) {
    if (Desc.External) {
      // 'External' functions are emitted as weak symbols.
      // This allows to override them in final elf.
      // Fuction bodies are filled later in FillExternalFunctionsStubsPass.
      auto &F = State.createFunction(M, Desc.Name, Sections.front(),
                                     Function::WeakAnyLinkage);
      auto *Node = CGS.emplaceNode(&F);
      Node->setExternal();
      NameMap.emplace(Desc.Name, Node);
    } else {
      auto IsEntryPoint = &Desc == &EntryPoint;
      CallGraphState::Node *N = nullptr;
      if (IsEntryPoint) {
        initRootFunctions(M, Desc.Name);
        N = CGS.getRootNode();
      } else {
        // All secondary functions are not assigned to specific RX section upon
        // creation. They are distributed to section later by
        // FunctionDistributePass.
        auto *NullSection = "";
        auto &MF = createFunction(
            SGCtx, M, Desc.Name, NullSection, Function::InternalLinkage,
            SGCtx.getGenSettings().Cfg.CGLayout.InstrNumAncil);
        N = CGS.emplaceNode(&(MF.getFunction()));
      }
      NameMap.emplace(Desc.Name, N);
    }
  }

  // Fill in connections.
  for (auto &Desc : Descs) {
    assert(NameMap.count(Desc.Name) && "missing entry for Desc");
    auto *N = NameMap.at(Desc.Name);
    for (auto &Callee : Desc.Callees) {
      assert(NameMap.count(Callee) && "missing entry for Callee");
      N->addCallee(NameMap.at(Callee));
    }
  }

  return true;
}

bool FunctionGenerator::generateDefault(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &CGS = get<GlobalCodeFlowInfo>().CGS;
  const auto &CGL = SGCtx.getGenSettings().getCallGraphLayout();
  // Create functions.
  auto NumF = CGL.FunctionNumber;

  assert(NumF && "Expected NumF >= 1");

  initRootFunctions(M, SGCtx.getProgramContext().getEntryPointName());

  iota_range<size_t> funIDs(0u, NumF - 1u, /*Inclusive*/ false);
  std::transform(
      funIDs.begin(), funIDs.end(), std::back_inserter(CGS), [&](auto ID) {
        // All secondary functions are not assigned to specific RX section upon
        // creation. They are distributed to section later by
        // FunctionDistributePass.
        auto *NullSection = "";
        auto &MF =
            createFunction(SGCtx, M, ("fun" + Twine(ID)).str(), NullSection,
                           Function::InternalLinkage,
                           SGCtx.getGenSettings().Cfg.CGLayout.InstrNumAncil);
        return &MF.getFunction();
      });

  // Fill in connections.

  // Algorithm in general:
  //   1. Each Node in graph is assigned a number(layer). Root node is assigned
  //      a value of -1, all other nodes get number in range [0, MaxLayer].
  //   2. Each node(A) is connected to another randomly selected node(B)
  //      if layer(B) > layer(A).
  //   3. Step 2. is repeated N times. N value is conigurable via
  //      GraphDensity option.
  //   4. If ForceConnect option is enabled, all nodes that happen to stay
  //      unreachable from root node after previous steps will get connected
  //      with a randomly selected reachable node accoring to rule in step 2.

  auto MaxLayer = CGL.MaxLayers;
  assert(MaxLayer && "MaxLayer must be at least 1");
  auto FunctionPerLayer = divideCeil(CGL.FunctionNumber - 1u, MaxLayer);
  if (FunctionPerLayer == 0)
    return true;

  // Step 1.

  SmallVector<LayerMapEntry, 5> LayerMap;
  auto *RootNode = CGS.getRootNode();
  LayerMap.emplace_back(RootNode, -1);

  auto NodeIt = std::next(CGS.nodes_begin());
  for (auto LayerIndex = 0u; LayerIndex < MaxLayer; ++LayerIndex) {
    for (auto FunctionIndex = 0u; FunctionIndex < FunctionPerLayer;
         ++FunctionIndex) {
      // Function count in last layer maybe overestimated.
      if (NodeIt == CGS.nodes_end())
        break;
      auto *Node = *NodeIt;
      LayerMap.emplace_back(Node, LayerIndex);
      ++NodeIt;
    }
  }

  // Steps 2. and 3.

  for (auto I = 0u; I < GraphDensity.getValue(); ++I) {
    RandEngine::shuffle(LayerMap.begin(), LayerMap.end());
    for (auto &&[Entry, NextEntry] :
         llvm::zip(ArrayRef(LayerMap.begin(), std::prev(LayerMap.end())),
                   ArrayRef(std::next(LayerMap.begin()), LayerMap.end()))) {
      if (NextEntry.Layer <= Entry.Layer ||
          Entry.Node->hasCallee(NextEntry.Node))
        continue;
      Entry.Node->addCallee(NextEntry.Node);
    }
  }

  // Step 4.

  if (ForceConnect) {
    DenseSet<CallGraphState::Node *> Reachables;
    fillReachableNodes(RootNode, Reachables);
    while (Reachables.size() != LayerMap.size()) {
      auto Unreachable = std::find_if(LayerMap.begin(), LayerMap.end(),
                                      [&Reachables](auto &Entry) {
                                        return !Reachables.contains(Entry.Node);
                                      });
      assert(Unreachable != LayerMap.end() &&
             "Must be at least one unreachable");
      auto EConnectTo = RandEngine::genNUniqInInterval(
          0ull, LayerMap.size() - 1ull, 1ull,
          [&LayerMap, &Unreachable, &Reachables](auto Index) {
            // Filter out Nodes of higher or equal layer and unreachable nodes.
            return LayerMap[Index].Layer >= Unreachable->Layer ||
                   !Reachables.count(LayerMap[Index].Node);
          });
      assert(EConnectTo && "Cannot create connection");
      auto ConnectTo = EConnectTo->front();

      LayerMap[ConnectTo].Node->addCallee(Unreachable->Node);
      fillReachableNodes(Unreachable->Node, Reachables);
    }
  }
  return true;
}

} // namespace snippy

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::CallGraphDumpMode,
                                           snippy::CallGraphDumpEnumOption)

} // namespace llvm
