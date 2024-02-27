//===-- FunctionGeneratorPass.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/Utils.h"
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

namespace {

struct FunctionGenerator final : public ModulePass {
public:
  static char ID;

  FunctionGenerator();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    ModulePass::getAnalysisUsage(AU);
  }

  bool readFromYaml(Module &M, const FunctionDescs &FDs);

  bool generateDefault(Module &M);

  std::vector<std::string> prepareRXSections();

  bool runOnModule(Module &M) override;
};

char FunctionGenerator::ID = 0;

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

INITIALIZE_PASS(FunctionGenerator, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createFunctionGeneratorPass() { return new FunctionGenerator(); }

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

FunctionGenerator::FunctionGenerator() : ModulePass(ID) {
  initializeFunctionGeneratorPass(*PassRegistry::getPassRegistry());
}

namespace {

MachineFunction &createFunction(GeneratorContext const &SGCtx, Module &M,
                                StringRef Name, StringRef SectionName,
                                Function::LinkageTypes Linkage) {

  auto &State = SGCtx.getLLVMState();
  std::string FinalName =
      SectionName.empty() || Linkage != Function::InternalLinkage
          ? std::string(Name)
          : (Twine(SectionName) + "." + Name).str();
  auto &MF = State.createMachineFunctionFor(
      State.createFunction(M, FinalName, SectionName, Linkage), SGCtx.getMMI());
  auto &Props = MF.getProperties();
  // FIXME: currently we don't keep liveness when creating and filling new BB
  if (SGCtx.hasCFInstrs() || SGCtx.hasCallInstrs())
    Props.reset(MachineFunctionProperties::Property::TracksLiveness);
  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);

  return MF;
}

} // namespace

// Get list of RX sections. Root functions must be placed to
// that sections in order. That is, entry function is assigned
// first section in list, exit function is assigned last section
// in list and all intermediate root functions goes between them
// in order.
std::vector<std::string> FunctionGenerator::prepareRXSections() {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &Linker = SGCtx.getLinker();

  if (!SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXSectionsFill)
    return {""};

  std::vector<std::string> Ret;
  for (auto &&[_, InputSections] : Linker.executionPath())
    Ret.emplace_back(InputSections.front());

  return Ret;
}

bool FunctionGenerator::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  auto &Ctx = State.getCtx();
  auto &LLVMTM = State.getTargetMachine();
  StringRef ABIName = SGCtx.getABIName();

  if (ABIName.size()) {
    auto *ABINameMD = MDString::get(Ctx, ABIName);
    M.setModuleFlag(Module::ModFlagBehavior::Error, "target-abi", ABINameMD);
  }

  M.setDataLayout(LLVMTM.createDataLayout());

  return !SGCtx.getConfig().FuncDescs.has_value()
             ? generateDefault(M)
             : readFromYaml(M, *SGCtx.getConfig().FuncDescs);
}

bool FunctionGenerator::readFromYaml(Module &M, const FunctionDescs &FDs) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  auto &Descs = FDs.Descs;
  auto &CGS = SGCtx.getCallGraphState();

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
      NameMap.emplace(Desc.Name, CGS.emplaceNode(&F));
    } else {
      auto IsEntryPoint = &Desc == &EntryPoint;
      CallGraphState::Node *N = nullptr;
      if (IsEntryPoint) {
        // Entry point produces multiple root functions. Each one of
        // them is assigned to respective RX section.
        auto &MF = createFunction(SGCtx, M, Desc.Name, Sections.front(),
                                  Function::ExternalLinkage);
        N = CGS.emplaceNode(&(MF.getFunction()));
        CGS.setRoot(N);
        for (auto &Section :
             llvm::make_range(std::next(Sections.begin()), Sections.end())) {
          CGS.appendNode(N, (&createFunction(SGCtx, M, Desc.Name, Section,
                                             Function::InternalLinkage)
                                  .getFunction()));
        }
      } else {
        // All secondary functions are not assigned to specific RX section upon
        // creation. They are distributed to section later by
        // FunctionDistributePass.
        auto *NullSection = "";
        auto &MF = createFunction(SGCtx, M, Desc.Name, NullSection,
                                  Function::InternalLinkage);
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
  auto &CGS = SGCtx.getCallGraphState();
  auto Sections = prepareRXSections();

  // Create functions.
  auto NumF = SGCtx.getCallGraphLayout().FunctionNumber;

  assert(NumF && "Expected NumF >= 1");

  auto &EntryFunction =
      createFunction(SGCtx, M, SGCtx.getEntryPointName(), Sections.front(),
                     Function::ExternalLinkage);

  auto *Root = CGS.emplaceNode(&(EntryFunction.getFunction()));
  CGS.setRoot(Root);

  // Create Root function for each section.
  for (auto &Section :
       llvm::make_range(std::next(Sections.begin()), Sections.end())) {
    auto &MF = createFunction(SGCtx, M, SGCtx.getEntryPointName(), Section,
                              Function::InternalLinkage);
    CGS.appendNode(Root, &MF.getFunction());
  }

  for (auto i = 0u; i < NumF - 1; ++i) {
    // All secondary functions are not assigned to specific RX section upon
    // creation. They are distributed to section later by
    // FunctionDistributePass.
    auto *NullSection = "";
    auto &MF = createFunction(SGCtx, M, ("fun" + Twine(i)).str(), NullSection,
                              Function::InternalLinkage);
    CGS.emplaceNode(&MF.getFunction());
  }

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

  auto MaxLayer = SGCtx.getCallGraphLayout().MaxLayers;
  assert(MaxLayer && "MaxLayer must be at least 1");
  auto FunctionPerLayer =
      divideCeil(SGCtx.getCallGraphLayout().FunctionNumber - 1u, MaxLayer);
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
    std::shuffle(LayerMap.begin(), LayerMap.end(), RandEngine::engine());
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
} // namespace llvm
