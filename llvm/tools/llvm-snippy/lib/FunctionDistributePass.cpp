//===-- FunctionDistributePass.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "llvm/CodeGen/MachineFunction.h"

#define DEBUG_TYPE "snippy-function-distribute"
#define PASS_DESC "Snippy Function Distribute"

namespace llvm {
namespace snippy {
namespace {

struct FunctionDistribute final : public ModulePass {
public:
  static char ID;

  FunctionDistribute() : ModulePass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<FunctionGenerator>();
    ModulePass::getAnalysisUsage(AU);
  }

  void calculateFunctionSizes(Module &M);

  void verifyFunctionSizes(Module &M, bool OnlyRootOnes) const;

  struct SectionSpaceInfo {
    std::string Name;
    size_t Capacity;
    size_t Used;
    SectionSpaceInfo(StringRef Name, size_t Cap, size_t Use)
        : Name(Name), Capacity(Cap), Used(Use){};
  };

  std::vector<SectionSpaceInfo> calculateAvailableSpace() const;

  bool runOnModule(Module &M) override;

private:
  std::unordered_map<const Function *, size_t> FunctionSizes;
};

char FunctionDistribute::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::FunctionDistribute;

INITIALIZE_PASS(FunctionDistribute, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

ModulePass *createFunctionDistributePass() { return new FunctionDistribute(); }

} // namespace llvm

namespace llvm {

namespace snippy {

void FunctionDistribute::verifyFunctionSizes(Module &M,
                                             bool OnlyRootOnes) const {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &ProgCtx = SGCtx.getProgramContext();
  auto &FG = getAnalysis<FunctionGenerator>();
  const auto &UsedSections = FG.get<GlobalCodeFlowInfo>().ExecutionPath;

  for (auto &Section : UsedSections) {
    std::vector<Function *> Functions;
    auto Examined = [&](auto &F) {
      return (!OnlyRootOnes || FG.isRootFunction(F)) &&
             ProgCtx.getOutputSectionFor(F).getIDString() ==
                 Section.OutputSection.Desc.getIDString();
    };
    for (auto &F : llvm::make_filter_range(M, Examined))
      Functions.emplace_back(&F);

    auto totalSize = std::accumulate(
        Functions.begin(), Functions.end(), 0ul,
        [this](auto Acc, auto *F) { return Acc + FunctionSizes.at(F); });
    if (totalSize <= Section.OutputSection.Desc.Size)
      continue;

    std::string Message;
    llvm::raw_string_ostream OS{Message};

    OS << "RX section '" << Section.OutputSection.Desc.getIDString()
       << "' (size " << Section.OutputSection.Desc.Size
       << ") failed to fit code mapped to it. Total code size: " << totalSize
       << "\n";
    OS << " List of functions mapped to this section:\n";
    for (auto *F : Functions) {
      OS << F->getName() << ": size " << FunctionSizes.at(F) << "\n";
    }

    OS << "Please, provide more space in RX sections or reduce instruction "
          "count.";

    snippy::fatal(StringRef(Message));
  }
}
void FunctionDistribute::calculateFunctionSizes(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getProgramContext().getLLVMState();
  for (auto &F : M)
    FunctionSizes.emplace(
        &F, State.getFunctionSize(
                SnippyModule::fromModule(M).getMMI().getOrCreateMachineFunction(
                    F)));
}

std::vector<FunctionDistribute::SectionSpaceInfo>
FunctionDistribute::calculateAvailableSpace() const {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &ProgCtx = SGCtx.getProgramContext();
  auto &RootFs = getAnalysis<FunctionGenerator>()
                     .getCallGraphState()
                     .getRootNode()
                     ->functions();
  std::vector<SectionSpaceInfo> Ret;

  for (auto *F : RootFs)
    Ret.emplace_back(ProgCtx.getOutputSectionName(*F),
                     ProgCtx.getOutputSectionFor(*F).Size, FunctionSizes.at(F));

  return Ret;
}

bool FunctionDistribute::runOnModule(Module &M) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &FG = getAnalysis<FunctionGenerator>();
  calculateFunctionSizes(M);

  // Don't do anything if singular section is used anyway.
  if (!SGCtx.getGenSettings().InstrsGenerationConfig.ChainedRXSectionsFill) {
    verifyFunctionSizes(M, /* OnlyRootOnes */ false);
    return false;
  }

  // Root functions have a pre-assigned RX section (see:
  // FunctionGeneratorPass.cpp). Check that their respective sections fit them.
  verifyFunctionSizes(M, /* OnlyRootOnes */ true);

  // First, calculate available space in every RX section.
  auto AvailableSpace = calculateAvailableSpace();

  // Second, arrange all secondary functions sorted from largest to smallest.
  std::vector<Function *> SortedBySize;
  llvm::transform(
      llvm::make_filter_range(
          M,
          [&M, &FG](auto &F) {
            return !FG.isRootFunction(
                SnippyModule::fromModule(M).getMMI().getOrCreateMachineFunction(
                    F));
          }),
      std::back_inserter(SortedBySize), [](auto &F) { return &F; });
  std::sort(SortedBySize.begin(), SortedBySize.end(),
            [this](auto *F1, auto *F2) {
              return FunctionSizes.at(F1) > FunctionSizes.at(F2);
            });

  // Then try to insert them one by one in specified order.
  for (auto *F : SortedBySize) {
    auto FSize = FunctionSizes.at(F);
    auto HasNoSpaceFor = [&](size_t Index) {
      auto &SpaceInfo = AvailableSpace.at(Index);
      return SpaceInfo.Capacity < SpaceInfo.Used + FSize;
    };

    // Pick random section that may fit this function.
    auto PlaceE = RandEngine::genNUniqInInterval(
        0ul, AvailableSpace.size() - 1ul, 1ul, HasNoSpaceFor);
    if (!PlaceE || PlaceE.get().empty())
      snippy::fatal(
          "Failed to fit secondary code in specified RX "
          "sections: not enough contiguos space found. Please, provide more "
          "space in RX sections or reduce instruction count.");

    auto &Section = AvailableSpace.at(PlaceE.get().front());
    F->setSection(Section.Name);

    // Update information about available space.
    Section.Used += FSize;
  }

  verifyFunctionSizes(M, /* OnlyRootOnes */ false);
  return true;
}

} // namespace snippy
} // namespace llvm
