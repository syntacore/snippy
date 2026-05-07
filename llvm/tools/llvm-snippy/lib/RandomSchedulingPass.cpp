//===-- RandomSchedulingPass.cpp    ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// RandomScheduling is post-processing pass for InstructionGenerator pass. This
/// pass permutes instructions within each MachineBasicBlock of MachineFunction
/// according to a random topological sort on its DDG
///
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GraphWriter.h"

namespace llvm {
template <>
struct DOTGraphTraits<ScheduleDAG *> : public DefaultDOTGraphTraits {

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(const ScheduleDAG *G) {
    return std::string(G->MF.getName());
  }

  static bool renderGraphFromBottomUp() { return true; }

  static bool isNodeHidden(const SUnit *Node, const ScheduleDAG *G) {
    return (Node->NumPreds > 10 || Node->NumSuccs > 10);
  }

  static std::string getNodeIdentifierLabel(const SUnit *Node,
                                            const ScheduleDAG *Graph) {
    std::string R;
    raw_string_ostream OS(R);
    OS << static_cast<const void *>(Node);
    return R;
  }

  static std::string getEdgeAttributes(const SUnit *Node, SUnitIterator EI,
                                       const ScheduleDAG *Graph) {
    if (EI.isArtificialDep())
      return "color=cyan,style=dashed";
    if (EI.isCtrlDep())
      return "color=blue,style=dashed";
    return "";
  }

  std::string getNodeLabel(const SUnit *SU, const ScheduleDAG *Graph);
  static std::string getNodeAttributes(const SUnit *N,
                                       const ScheduleDAG *Graph) {
    return "shape=Mrecord";
  }

  static void addCustomGraphFeatures(ScheduleDAG *G,
                                     GraphWriter<ScheduleDAG *> &GW) {
    return G->addCustomGraphFeatures(GW);
  }
};
} // namespace llvm

namespace llvm::snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string>
    DumpSchedDAGName("dump-sched-dag",
                     cl::desc("specify a title for dot-dump scheduled basick "
                              "block (under LLVM_DEBUG)"),
                     cl::value_desc("imagetitle"), cl::Hidden, cl::cat(Options),
                     cl::init("DAG"));
namespace {

#define DEBUG_TYPE "snippy-random-scheduling"
#define PASS_DESC "Snippy-random-scheduling"

class RandomScheduling final : public MachineFunctionPass {
  // We use ScheduleDAGInstrs routine to build DDG.
  // Then we implement the algorithm of getting random topology sorting:
  // Algorithm description:
  // Let's look at the following graph:
  //                      i1       i7
  //                     /  \      |
  //                   i2    i3    i8
  //                  / | \
  //                i4  i5 \
  //                        i6
  // SUnits : [i1, i2, i3, i4, i5, i6, i7, i8]
  // We want to collect a random topological sequence RandomTopologicalSUnitsSeq
  // : [] First of all, we need to pick out all SUnits without any predecessors:
  // ZeroPredUnits : [i1, i7]
  // Then we choose random one SUnit from ZeroPredUnits. Suppose we choose i1.
  // After this, we need to remove edges between i1 and {i2, i3}
  // Then, i2 and i3 nodes have no predecessors, so we can add them into
  // ZeroPredUnits and remove i1 from the candiadates ZeroPredUnits : [i7, i2,
  // i3] Then, put i1 to RandomTopologicalSUnitsSeq : [i1] Next itteration will
  // happen with the transformed graph:
  //                   i2    i3    i7
  //                  / | \        |
  //                i4  i5 \       i8
  //                        i6
  // Repeat all actions...
  // The algorithm stops when ZeroPredUnits becomes empty
  // In this case possible random topological sequence:
  // RandomTopologicalSUnitsSeq: [i1, i3, i7, i2, i5, i8, i4, i6]
  class SnippyRandomScheduler : public ScheduleDAGInstrs {
    using ItType = MachineBasicBlock::iterator;
    // Vector of regions to schedule (with Begin / End iterators)
    using RegionsToSchedTy = SmallVector<std::pair<ItType, ItType>>;

    TargetLibraryInfoImpl TLIImpl;
    TargetLibraryInfo TLI;
    AAResults AA;
    const SnippyProgramContext &ProgCtx;
    unsigned TheMaxRegionSize;

    std::vector<std::reference_wrapper<SUnit>> getRandomTopologicalSort() {
      std::vector<std::reference_wrapper<SUnit>> RandomTopologicalSUnitsSeq;
      std::vector<std::reference_wrapper<SUnit>> ZeroPredUnits;

      std::copy_if(SUnits.begin(), SUnits.end(),
                   std::back_inserter(ZeroPredUnits),
                   [](const auto &SU) { return SU.Preds.empty(); });

      UniformIntDistribution<unsigned> Dist;
      auto &Engine = RandEngine::engine();
      while (!ZeroPredUnits.empty()) {
        // Choose a random one from ZeroPredUnits
        auto Idx = Dist(Engine) % ZeroPredUnits.size();
        auto RandomSUnitFromCandidates = ZeroPredUnits[Idx];
        // Delete current SUnit form preds of all its succs
        for (auto &&Succ : RandomSUnitFromCandidates.get().Succs) {
          auto *SuccSUnit = Succ.getSUnit();
          assert(SuccSUnit);

          auto It =
              std::find_if(SuccSUnit->Preds.begin(), SuccSUnit->Preds.end(),
                           [RandomSUnitFromCandidates](const auto &SuccPred) {
                             return SuccPred.getSUnit()->getInstr() ==
                                    RandomSUnitFromCandidates.get().getInstr();
                           });
          assert(It != SuccSUnit->Preds.end());
          SuccSUnit->Preds.erase(It);

          // If the succ's SUnit has no preds any more, we can add it to
          // ZeroPredUnits
          if (SuccSUnit->Preds.empty())
            ZeroPredUnits.push_back(*SuccSUnit);
        }
        // Added handled SUnit to the topological sequence
        RandomTopologicalSUnitsSeq.push_back(RandomSUnitFromCandidates);
        // As soon as we handle SUnit, we need to remove it from candidates
        std::swap(ZeroPredUnits[Idx], ZeroPredUnits.back());
        ZeroPredUnits.pop_back();
      }

      return RandomTopologicalSUnitsSeq;
    }

    void reorderBasicBlock(MachineBasicBlock &MBB,
                           const std::vector<std::reference_wrapper<SUnit>>
                               &RandomTopologicalSUnitsSeq) const {
      for (auto &&Unit : RandomTopologicalSUnitsSeq) {
        auto It = Unit.get().getInstr()->getIterator();
        assert(It != MBB.end());
        MBB.splice(end(), &MBB, It);
      }
    }

    auto getRegionsForMBBScheduling(MachineBasicBlock &MBB) {
      // Instructions marked with only Bundle metadata are not subject to
      // scheduling. However, such bundle groups are often preceded by support
      // instruction groups (e.g., for address formation). These support groups
      // can be scheduled within their own boundaries. Therefore, we schedule
      // them separately
      auto IsSupportBundleInstr = [](auto &&Instr) {
        return checkMetadata(Instr, SnippyMetadata::Bundle) &&
               checkMetadata(Instr, SnippyMetadata::Support);
      };
      RegionsToSchedTy SupportRegionsToSched;
      for (auto Begin = MBB.begin(), End = MBB.end(); Begin != End;) {
        auto SupportRangeStart = Begin;
        Begin = std::find_if_not(Begin, End, IsSupportBundleInstr);
        if (SupportRangeStart != Begin)
          SupportRegionsToSched.emplace_back(SupportRangeStart, Begin);
        else
          ++Begin;
      }
      const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
      RegionsToSchedTy Regions;
      for (auto &&[SuppBegin, SuppEnd] : SupportRegionsToSched)
        fillSchedulingRegions(SuppBegin, SuppEnd, SnippyTgt, Regions);
      // Schedule all the remaining instructions, no longer taking into account
      // the support instructions for the bundles
      fillSchedulingRegions(MBB.begin(), MBB.end(), SnippyTgt, Regions);
      return Regions;
    }

    void fillSchedulingRegions(ItType BeginIt, ItType EndIt,
                               const SnippyTarget &SnippyTgt,
                               RegionsToSchedTy &Regions) const {
      static constexpr auto MinimalRegionRange = 3u;

      auto MayBeSched = [&SnippyTgt](const auto &MI) {
        return SnippyTgt.mayBeScheduled(MI);
      };

      // This is basically chunking by a predicate with an upper limit on the
      // chunk size.
      auto FindIfWithUpperSizeLimit = [&](auto Begin, auto &&Pred) {
        size_t Count = 0;
        for (Count = 0;
             Begin != EndIt && Count < TheMaxRegionSize && !Pred(*Begin);
             ++Count, ++Begin) {
        }
        return std::pair{Begin, Count};
      };

      while ((BeginIt = FindIfWithUpperSizeLimit(BeginIt, MayBeSched).first) !=
             EndIt) {
        auto [CurEnd, Count] =
            FindIfWithUpperSizeLimit(BeginIt, std::not_fn(MayBeSched));
        if (Count > MinimalRegionRange)
          Regions.emplace_back(BeginIt, CurEnd);
        BeginIt = CurEnd;
      }
    }

    template <typename ItType>
    void buildDDGForRegion(ItType RegStart, ItType RegEnd,
                           MachineBasicBlock &MBB) {
      auto Begin = RegStart;
      assert(Begin != RegEnd);
      auto End = RegEnd;

      enterRegion(&MBB, Begin, End, std::distance(Begin, End));
      buildSchedGraph(&AA);
    }

    void buildDDGForMBB(MachineBasicBlock &MBB) {
      startBlock(&MBB);
      auto Begin = MBB.begin();
      assert(Begin != MBB.end());
      auto End = MBB.getFirstTerminator();
      // Last instruction in a some basic block can be custom or interrupt one
      if (End == MBB.end())
        End = std::prev(MBB.end());
      assert(End != MBB.end());

      enterRegion(&MBB, Begin, End, std::distance(Begin, End));
      buildSchedGraph(&AA);
    }

  public:
    SnippyRandomScheduler(MachineFunction &MF, const MachineLoopInfo *MLI,
                          const SnippyProgramContext &ProgCtx,
                          unsigned MaxRegionSize)
        : ScheduleDAGInstrs(MF, MLI, /* RemoveKillFlags */ false),
          TLIImpl(ProgCtx.getLLVMState().getTargetMachine().getTargetTriple()),
          TLI(TLIImpl), AA(TLI), ProgCtx(ProgCtx),
          TheMaxRegionSize(MaxRegionSize) {}

    void scheduleBasicBlock(MachineBasicBlock &MBB) {
      auto Regions = getRegionsForMBBScheduling(MBB);
      LLVM_DEBUG(buildDDGForMBB(MBB); dumpDotGraphToFile(
                     cast<ScheduleDAG>(this),
                     DumpSchedDAGName.getValue() + "-" + MBB.getFullName(),
                     "ScedulingPass"););
      // Required by buildSchedGraph method defined in ScheduleDAGInstrs
      startBlock(&MBB);
      for (auto &&Reg : Regions) {
        buildDDGForRegion(Reg.first, Reg.second, MBB);
        auto &&RandomTopologicalSUnitsSeq = getRandomTopologicalSort();
        // Reorganization of BB after scheduling
        [[maybe_unused]] auto InitBBSize = MBB.size();
        reorderBasicBlock(MBB, RandomTopologicalSUnitsSeq);
      }
      LLVM_DEBUG(buildDDGForMBB(MBB);
                 dumpDotGraphToFile(cast<ScheduleDAG>(this),
                                    DumpSchedDAGName.getValue() + "-" +
                                        MBB.getFullName() + "-scheduled",
                                    "ScedulingPass"););
    }

    void schedule() override {
      auto &RegInfo = MF.getRegInfo();
      RegInfo.freezeReservedRegs();

      for (auto &&MBB : MF)
        scheduleBasicBlock(MBB);
    }
  };

public:
  static char ID;

  RandomScheduling(unsigned MaxRegionSize)
      : MachineFunctionPass(ID), TheMaxRegionSize(MaxRegionSize) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
    auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();

    MF.getProperties().set(MachineFunctionProperties::Property::TracksLiveness);

    SnippyRandomScheduler Scheduler(MF, &MLI, SGCtx.getProgramContext(),
                                    TheMaxRegionSize);
    Scheduler.schedule();
    return true;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  unsigned TheMaxRegionSize;
};

char RandomScheduling::ID = 0;

} // namespace
} // namespace llvm::snippy

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::RandomScheduling;

INITIALIZE_PASS_BEGIN(RandomScheduling, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(RandomScheduling, DEBUG_TYPE, PASS_DESC, false, false)
namespace llvm {
MachineFunctionPass *createRandomSchedulingPass(unsigned MaxRegionSize) {
  return new RandomScheduling(MaxRegionSize);
}
} // namespace llvm
