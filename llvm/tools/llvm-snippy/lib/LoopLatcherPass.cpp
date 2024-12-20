//===-- LoopLatcherPass.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// LoopLatcher is a pass to guarantee number of loops iterations.
///
/// Algorithm in general:
///   1. Insert loop init in preheader;
///   2. Insert loop iteration instructions in a control block of the loop.
///
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/CFPermutationPass.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/LoopLatcherPass.h"
#include "snippy/Generator/RegReservForLoop.h"
#include "snippy/Generator/RootRegPoolWrapperPass.h"
#include "snippy/Generator/SimulatorContextWrapperPass.h"
#include "snippy/Support/Options.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

#define DEBUG_TYPE "snippy-loop-latcher"
#define PASS_DESC "Snippy Loop Latcher"

static snippy::opt<bool>
    UseStackOpt("use-stack-for-IV",
                cl::desc("Place induction variables on stack."),
                cl::cat(Options), cl::Hidden);

void SnippyLoopInfo::addIncomingValues(const MachineBasicBlock *MBB,
                                       RegToValueType RegToValue) {
  assert(MBB);
  assert(IncomingValues.count(MBB) == 0);
  IncomingValues[MBB] = std::move(RegToValue);
}

const SnippyLoopInfo::RegToValueType &
SnippyLoopInfo::getIncomingValues(const MachineBasicBlock *MBB) const {
  assert(MBB);
  assert(IncomingValues.count(MBB));
  return IncomingValues.at(MBB);
}

//       Initialize consecutive loop in previous loop:
//
//       +------v------+
//       | loop1 init  |
//       +------+------+
//              | _________
//       loop1  |/         \
//       +------v------+   |
//       | loop2 init  |   |
//       | loop1 latch |   |
//       +------+------+   |
//              |\_________/
//              | _________
//       loop2  |/         \
//       +------v------+   |
//       | loop2 latch |   |
//       +------+------+   |
//              |\_________/
//              |
//              |
//       +------v------+
//       |             |
//       +------+------+
static snippy::opt<bool> InitConsLoopInPrevLoop(
    "init-cons-loop-in-prev-loop",
    cl::desc("Initialize consecutive loop in previous loop."),
    cl::cat(Options));

LoopLatcher::LoopLatcher()
    : ActiveImmutablePass<MachineFunctionPass, SnippyLoopInfo>(ID) {}

StringRef LoopLatcher::getPassName() const { return PASS_DESC " Pass"; }

void LoopLatcher::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GeneratorContextWrapper>();
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addRequired<CFPermutation>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addRequired<MachinePostDominatorTreeWrapperPass>();
  AU.addRequired<SimulatorContextWrapper>();
  AU.addRequired<RootRegPoolWrapper>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

char LoopLatcher::ID = 0;

} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LoopLatcher;

SNIPPY_INITIALIZE_PASS(LoopLatcher, DEBUG_TYPE, PASS_DESC, false)

namespace llvm {

snippy::ActiveImmutablePassInterface *createLoopLatcherPass() {
  return new LoopLatcher();
}

} // namespace llvm

namespace llvm {
namespace snippy {
bool LoopLatcher::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(
      dbgs() << "MachineFunction at the start of llvm::snippy::LoopLatcher:\n";
      MF.dump());
  auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  if (MLI.empty()) {
    LLVM_DEBUG(dbgs() << "No loops in function, exiting.\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "Machine Loop Info for this function:\n");
  LLVM_DEBUG(MLI.print(dbgs()));

  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  const auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &CLI = getAnalysis<CFPermutation>().get<ConsecutiveLoopInfo>(MF);
  if (UseStackOpt && !ProgCtx.stackEnabled())
    snippy::fatal(State.getCtx(),
                  "Cannot place IVs on the stack:", " stack was not enabled.");

  if (SimCtx.hasTrackingMode() && !ProgCtx.stackEnabled())
    snippy::fatal(
        State.getCtx(), "Wrong snippy configuration:",
        "loops generation in selfcheck and backtracking modes requires stack.");

  for (auto *ML : MLI) {
    assert(ML);
    auto HeaderNumber = ML->getHeader()->getNumber();
    if (CLI.isFirstConsecutiveLoopHeader(HeaderNumber))
      createLoopLatchFor(*ML, CLI.getConsecutiveLoops(HeaderNumber));
    else if (!CLI.isNonFirstConsecutiveLoopHeader(HeaderNumber))
      createLoopLatchFor(*ML);
  }

  bool Changed = !MLI.empty();
  return Changed;
}

template <bool IsPostDom>
void processDomTree(
    const MachineBasicBlock &MBBToProcess, const MachineBasicBlock &MBBToCheck,
    const DominatorTreeBase<MachineBasicBlock, IsPostDom> &MainDomTree,
    const DominatorTreeBase<MachineBasicBlock, !IsPostDom> &CheckDomTree,
    std::insert_iterator<std::set<const MachineBasicBlock *, MIRComp>>
        &&Reserv) {
  SmallVector<const MachineBasicBlock *> DominatedBySelectedMBB = {
      &MBBToProcess};

  while (!DominatedBySelectedMBB.empty()) {
    auto *Last = DominatedBySelectedMBB.back();
    DominatedBySelectedMBB.pop_back();
    if (CheckDomTree.dominates(&MBBToCheck, Last))
      Reserv = Last;
    transform(make_filter_range(MainDomTree[Last]->children(),
                                [&MBBToCheck](auto &&DomNode) {
                                  return DomNode->getBlock() != &MBBToCheck;
                                }),
              std::back_inserter(DominatedBySelectedMBB),
              [](auto &&DomNode) { return DomNode->getBlock(); });
  }
}

auto LoopLatcher::selectRegsForBranch(const MCInstrDesc &BranchDesc,
                                      const MachineBasicBlock &Preheader,
                                      const MachineBasicBlock &ExitingBlock,
                                      const MCRegisterClass &RegClass) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RootPool = getAnalysis<RootRegPoolWrapper>().getPool();

  auto ImmutableReg = SnippyTgt.getImmutableRegs(RegClass);
  auto Filter = [&ImmutableReg](unsigned Reg) {
    auto *Pos = find(ImmutableReg, Reg);
    return Pos != ImmutableReg.end();
  };

  auto &DomTree = getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  auto &PostDomTree =
      getAnalysis<MachinePostDominatorTreeWrapperPass>().getPostDomTree();

  assert(DomTree.dominates(&Preheader, &ExitingBlock));
  assert(PostDomTree.dominates(&ExitingBlock, &Preheader));

  std::set<const MachineBasicBlock *, MIRComp> MBBsForReserv;

  // We need to make reservation for all blocks that are dominated by Preheader
  // and postdominated by Exiting block
  processDomTree(Preheader, ExitingBlock, DomTree, PostDomTree,
                 std::inserter(MBBsForReserv, MBBsForReserv.end()));
  processDomTree(ExitingBlock, Preheader, PostDomTree, DomTree,
                 std::inserter(MBBsForReserv, MBBsForReserv.end()));

  auto NumRegsToReserv = SnippyTgt.getNumRegsForLoopBranch(BranchDesc);

  auto RegsToReserv = RootPool.getNAvailableRegisters(
      "for loop latch", RegInfo, RegClass, MBBsForReserv, NumRegsToReserv,
      Filter, AccessMaskBit::RW);

  for (auto &&Reg : RegsToReserv)
    RootPool.addReserved(Reg, Preheader, AccessMaskBit::W);

  auto TrackingMode = SimCtx.hasTrackingMode();
  if (UseStackOpt || TrackingMode) {
    // We still have to reserve counter register even when using the stack.
    // Otherwise, this register might be later reserved for other purposes in
    // flow generator (or any other part of snippy) and we'll corrupt the data
    // stored in it. Reservation can be smaller though - only one block.
    auto ReservationMode = TrackingMode ? AccessMaskBit::RW : AccessMaskBit::W;

    for (auto &&Reg : RegsToReserv)
      RootPool.addReserved(Reg, ExitingBlock, ReservationMode);

  } else {
    for (const auto *MBB : MBBsForReserv)
      for (auto &&Reg : RegsToReserv)
        RootPool.addReserved(Reg, *MBB, AccessMaskBit::W);
  }

  SmallVector<Register> ReservedRegs;
  std::copy(RegsToReserv.begin(), RegsToReserv.end(),
            std::back_inserter(ReservedRegs));
  return ReservedRegs;
}

static void printSelectedRegs(
    raw_ostream &OS, ArrayRef<Register> ReservedRegs,
    const MCRegisterInfo &RegInfo,
    std::optional<const MCRegisterClass *> MCRegClass = std::nullopt) {
  auto CounterReg = ReservedRegs[CounterRegIdx];
  if (MCRegClass.has_value()) {
    OS << "Selected regs: " << RegInfo.getRegClassName(MCRegClass.value())
       << ": " << RegInfo.getName(CounterReg) << "[CounterReg]";
    if (ReservedRegs.size() == MaxNumOfReservRegsForLoop) {
      auto LimitReg = ReservedRegs[LimitRegIdx];
      OS << " & " << RegInfo.getName(LimitReg) << "[LimitReg]";
    }
  } else {
    OS << RegInfo.getName(CounterReg) << "(" << CounterReg << ")[CounterReg]";
    if (ReservedRegs.size() == MaxNumOfReservRegsForLoop) {
      auto LimitReg = ReservedRegs[LimitRegIdx];
      OS << ", " << RegInfo.getName(LimitReg) << "(" << LimitReg
         << ")[LimitReg]";
    }
  }
  OS << "\n";
}

MachineInstr &LoopLatcher::updateLatchBranch(MachineLoop &ML,
                                             MachineInstr &Branch,
                                             MachineBasicBlock &Preheader,
                                             ArrayRef<Register> ReservedRegs) {
  assert(Branch.isConditionalBranch() && "Conditional branch expected");
  assert(ML.contains(&Branch) && "Expected this loop branch");

  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &BranchDesc = InstrInfo.get(Branch.getOpcode());
  const auto &SnippyTgt = State.getSnippyTarget();

  LLVM_DEBUG(dbgs() << "Old branch: "; Branch.dump());
  auto &NewBranch =
      SnippyTgt.updateLoopBranch(Branch, BranchDesc, ReservedRegs);
  LLVM_DEBUG(dbgs() << "New branch: "; NewBranch.dump());
  return NewBranch;
}

void LoopLatcher::processExitingBlock(MachineLoop &ML,
                                      MachineBasicBlock &ExitingBlock,
                                      MachineBasicBlock &Preheader) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto SimCtx = getAnalysis<SimulatorContextWrapper>()
                    .get<OwningSimulatorContext>()
                    .get();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto TrackingMode = SimCtx.hasTrackingMode();

  auto FirstTerm = ExitingBlock.getFirstTerminator();
  assert(FirstTerm != ExitingBlock.end() &&
         "ExitingBlock expected to have terminator");
  assert(FirstTerm->isConditionalBranch() &&
         "ExitingBlock expected to have conditional "
         "branch as first terminator");
  auto &Branch = *FirstTerm;
  const auto &InstrInfo = State.getInstrInfo();
  const auto &BranchDesc = InstrInfo.get(Branch.getOpcode());
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &MCRegClass = SnippyTgt.getMCRegClassForBranch(ProgCtx, Branch);

  auto ReservedRegs =
      selectRegsForBranch(BranchDesc, Preheader, ExitingBlock, MCRegClass);

  assert((ReservedRegs.size() >= MinNumOfReservRegsForLoop) &&
         (ReservedRegs.size() <= MaxNumOfReservRegsForLoop) &&
         "One or Two Registers expected to be reserved for branch");

  LLVM_DEBUG(
      printSelectedRegs(dbgs(), ReservedRegs, State.getRegInfo(), &MCRegClass));

  auto &NewBranch = updateLatchBranch(ML, Branch, Preheader, ReservedRegs);
  Branch.removeFromParent();

  assert(SGCtx.getConfig().Branches.NLoopIter.Min);
  assert(SGCtx.getConfig().Branches.NLoopIter.Max);
  auto NIterMin = *SGCtx.getConfig().Branches.NLoopIter.Min;
  auto NIterMax = *SGCtx.getConfig().Branches.NLoopIter.Max;
  auto NIter = RandEngine::genInInterval(NIterMin, NIterMax);
  LLVM_DEBUG(dbgs() << "Loop counter init inserting: " << NIter
                    << " iterations, ");
  LLVM_DEBUG(printSelectedRegs(dbgs(), ReservedRegs, State.getRegInfo()));

  auto PreheaderInsertPt = Preheader.getFirstTerminator();
  InstructionGenerationContext PHCtx{Preheader, PreheaderInsertPt, SGCtx,
                                     SimCtx};
  auto MinLoopCountVal =
      SnippyTgt.insertLoopInit(PHCtx, NewBranch, ReservedRegs, NIter);

  auto SP = ProgCtx.getStackPointer();
  if (UseStackOpt || TrackingMode) {
    for (auto &&Reg : ReservedRegs)
      SnippyTgt.generateSpillToStack(PHCtx, Reg, SP);
  }

  LLVM_DEBUG(dbgs() << "Loop counter init inserted: "; Preheader.dump());

  auto *Header = ML.getHeader();
  assert(Header);

  // Currently this is a workaround specifically for selfcheck and placing IV's
  // on the stack. Ind-var schemes assume that the IV value is stored in the
  // register, which is not true when UseStackOpt is set. In case of non-trivial
  // loops IV value should be read from the stack so that only one register is
  // used for deeply nested loops. This is currently not implemented so
  // UseStackOpt should not be used together with IV addressing schemes.
  MachineBasicBlock::iterator InsPos =
      UseStackOpt || TrackingMode ? NewBranch : Header->getFirstNonPHI();

  auto *InsMBB = InsPos->getParent();
  assert(InsMBB);
  InstructionGenerationContext HeadCtx{*InsMBB, InsPos, SGCtx, SimCtx};

  if (UseStackOpt || TrackingMode) {
    for (auto &&Reg : reverse(ReservedRegs))
      SnippyTgt.generateReloadFromStack(HeadCtx, Reg, SP);
  }

  // FIXME: Currently selfcheck mode really does not behave well when loop
  // counter gets incremented at the beginning of the header. However, ideally
  // increment needs to either precede or happen after all of the instructions
  // in the loop body. Otherwise it's not obvious how to generate strided
  // accesses. If the increment happends somewhere in the middle of the loop
  // body (as it does now with selfcheck) off-by-one errors are possible. So,
  // when using indvars for addressing increment should be moved to the top of
  // the header.
  // TLDR: Selfcheck assumtions are too lax and at some point it should be
  // fixed to work with non-trivial loops.

  RegToValueType ExitingValues;
  auto CounterInsRes = SnippyTgt.insertLoopCounter(
      HeadCtx, NewBranch, ReservedRegs, NIter, ExitingValues, MinLoopCountVal);
  auto &Diag = CounterInsRes.Diag;
  auto ActualNumIter = CounterInsRes.NIter;
  unsigned MinCounterVal = CounterInsRes.MinCounterVal.getZExtValue();
  auto CounterReg = ReservedRegs[CounterRegIdx];
  auto &SLI = get<SnippyLoopInfo>(*ML.getHeader()->getParent());
  SnippyLoopInfo::LoopGenerationInfo TheLoopGenInfo{
      CounterReg, ActualNumIter, MinCounterVal,
      SnippyTgt.getLoopType(NewBranch)};
  SLI.addLoopGenerationInfoForMBB(ML.getHeader(), TheLoopGenInfo);

  if (UseStackOpt || TrackingMode) {
    auto *Exit = ML.getExitBlock();
    assert(Exit);
    SLI.addIncomingValues(Exit, std::move(ExitingValues));
    for (auto &&Reg : ReservedRegs)
      SnippyTgt.generateSpillToStack(HeadCtx, Reg, SP);
    InstructionGenerationContext ExitCtx{*Exit, Exit->getFirstNonPHI(), SGCtx,
                                         SimCtx};
    for (auto &&Reg : reverse(ReservedRegs))
      SnippyTgt.generatePopNoReload(ExitCtx, Reg);
  }

  if (Diag.has_value() &&
      (Diag.value().getName() != WarningName::LoopIterationNumber ||
       !NIterWarned)) {
    State.getCtx().diagnose(Diag.value());
    if (Diag.value().getName() == WarningName::LoopIterationNumber)
      NIterWarned = true;
  }

  LLVM_DEBUG(dbgs() << "Loop counter inserted: "; ExitingBlock.dump());
}

bool LoopLatcher::createLoopLatchFor(MachineLoop &ML) {
  LLVM_DEBUG(dbgs() << "Creating latch for "; ML.dump());
  auto *Exiting = ML.getExitingBlock();
  assert(Exiting && "Expected to have only one exiting block.");
  LLVM_DEBUG(dbgs() << "Loop exiting block found:\n"; Exiting->dump(););

  auto *Preheader = ML.getLoopPreheader();
  assert(Preheader && "Loop must already have preheader");
  processExitingBlock(ML, *Exiting, *Preheader);

  for_each(ML.getSubLoops(), [this](auto *SubLoop) {
    assert(SubLoop);
    createLoopLatchFor(*SubLoop);
  });

  return true;
}

template <typename R>
bool LoopLatcher::createLoopLatchFor(MachineLoop &ML, R &&ConsecutiveLoops) {
  LLVM_DEBUG(dbgs() << "Creating latch for "; ML.dump());
  LLVM_DEBUG(dbgs() << "  and it's sequential loops:");
  LLVM_DEBUG(
      for_each(ConsecutiveLoops, [](auto &&BB) { dbgs() << " " << BB; }));
  auto *Exiting = ML.getExitingBlock();
  assert(Exiting && "Expected to have only one exiting block.");
  LLVM_DEBUG(dbgs() << "Loop exiting block found:\n"; Exiting->dump(););

  auto *Preheader = ML.getLoopPreheader();
  assert(Preheader && "Loop must already have preheader");
  processExitingBlock(ML, *Exiting, *Preheader);

  assert(ML.getSubLoops().empty() &&
         "First consecutive loop must not have sub loop");

  auto &MF = *Preheader->getParent();
  auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  for (auto &&BBNum : ConsecutiveLoops) {
    auto *ConsLoop = MLI.getLoopFor(MF.getBlockNumbered(BBNum));
    assert(ConsLoop);
    LLVM_DEBUG(dbgs() << "Creating latch for consecutive loop: ";
               ConsLoop->dump());
    auto *ConsLoopExiting = ConsLoop->getExitingBlock();
    assert(ConsLoopExiting);
    auto *ConsLoopPreheader =
        InitConsLoopInPrevLoop ? ConsLoopExiting->getPrevNode() : Preheader;
    processExitingBlock(*ConsLoop, *ConsLoopExiting, *ConsLoopPreheader);
  }

  return true;
}

} // namespace snippy
} // namespace llvm
