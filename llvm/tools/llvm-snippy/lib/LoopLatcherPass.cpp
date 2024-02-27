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

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"
#include "RootRegPoolWrapperPass.h"

#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Support/Error.h"
#include "snippy/Support/Options.h"
#include "snippy/Target/Target.h"

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

namespace {

#define DEBUG_TYPE "snippy-loop-latcher"
#define PASS_DESC "Snippy Loop Latcher"

snippy::opt<bool> UseStackOpt("use-stack-for-IV",
                              cl::desc("Place induction variables on stack."),
                              cl::cat(Options), cl::Hidden);

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
snippy::opt<bool> InitConsLoopInPrevLoop(
    "init-cons-loop-in-prev-loop",
    cl::desc("Initialize consecutive loop in previous loop."),
    cl::cat(Options));

class LoopLatcher final : public MachineFunctionPass {
  void processExitingBlock(MachineLoop &ML, MachineBasicBlock &ExitingBlock,
                           MachineBasicBlock &Preheader);
  bool createLoopLatchFor(MachineLoop &ML);
  template <typename R>
  bool createLoopLatchFor(MachineLoop &ML, R &&ConsecutiveLoops);
  auto selectRegsForBranch(const MachineLoop &ML,
                           const MachineBasicBlock &Preheader,
                           const MachineBasicBlock &ExitingBlock,
                           const MCRegisterClass &RegClass);
  MachineInstr &updateLatchBranch(MachineLoop &ML, MachineInstr &Branch,
                                  MachineBasicBlock &Preheader,
                                  unsigned CounterReg, unsigned LimitReg);

  bool NIterWarned = false;

public:
  static char ID;

  LoopLatcher();

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<MachineDominatorTree>();
    AU.addRequired<MachinePostDominatorTree>();
    AU.addRequired<RootRegPoolWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char LoopLatcher::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::LoopLatcher;

INITIALIZE_PASS_BEGIN(LoopLatcher, DEBUG_TYPE, PASS_DESC, false, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTree)
INITIALIZE_PASS_DEPENDENCY(RootRegPoolWrapper)
INITIALIZE_PASS_END(LoopLatcher, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createLoopLatcherPass() { return new LoopLatcher(); }

} // namespace llvm

namespace llvm {
namespace snippy {

LoopLatcher::LoopLatcher() : MachineFunctionPass(ID) {
  initializeLoopLatcherPass(*PassRegistry::getPassRegistry());
}

bool LoopLatcher::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(
      dbgs() << "MachineFunction at the start of llvm::snippy::LoopLatcher:\n";
      MF.dump());
  auto &MLI = getAnalysis<MachineLoopInfo>();
  if (MLI.empty()) {
    LLVM_DEBUG(dbgs() << "No loops in function, exiting.\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "Machine Loop Info for this function:\n");
  LLVM_DEBUG(MLI.getBase().print(dbgs()));

  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  if (UseStackOpt && !SGCtx.stackEnabled())
    snippy::fatal(State.getCtx(),
                  "Cannot place IVs on the stack:", " stack was not enabled.");

  if (SGCtx.hasTrackingMode() && !SGCtx.stackEnabled())
    snippy::fatal(
        State.getCtx(), "Wrong snippy configuration:",
        "loops generation in selfcheck and backtracking modes requires stack.");

  for (auto *ML : MLI) {
    assert(ML);
    auto HeaderNumber = ML->getHeader()->getNumber();
    if (SGCtx.isFirstConsecutiveLoopHeader(HeaderNumber))
      createLoopLatchFor(*ML, SGCtx.getConsecutiveLoops(HeaderNumber));
    else if (!SGCtx.isNonFirstConsecutiveLoopHeader(HeaderNumber))
      createLoopLatchFor(*ML);
  }

  bool Changed = !MLI.empty();
  return Changed;
}

static const auto &getMCRegClassForBranch(const MCInstrDesc &InstrDesc,
                                          const MCRegisterInfo &RegInfo) {
  assert(InstrDesc.isBranch() && "Branch expected");
  auto *RegOperand = std::find_if(
      InstrDesc.operands().begin(), InstrDesc.operands().end(),
      [](const auto &OpInfo) {
        return OpInfo.OperandType == MCOI::OperandType::OPERAND_REGISTER;
      });
  assert(
      RegOperand != InstrDesc.operands().end() &&
      "All supported branches expected to have at least one register operand");
  return RegInfo.getRegClass(RegOperand->RegClass);
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

auto LoopLatcher::selectRegsForBranch(const MachineLoop &ML,
                                      const MachineBasicBlock &Preheader,
                                      const MachineBasicBlock &ExitingBlock,
                                      const MCRegisterClass &RegClass) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  auto &RegInfo = State.getRegInfo();
  const auto &SnippyTgt = State.getSnippyTarget();
  auto &RootPool = getAnalysis<RootRegPoolWrapper>().getPool();

  auto ImmutableReg = SnippyTgt.getImmutableRegs(RegClass);
  auto Filter = [&ImmutableReg](unsigned Reg) {
    auto *Pos = find(ImmutableReg, Reg);
    return Pos != ImmutableReg.end();
  };

  auto &DomTree = getAnalysis<MachineDominatorTree>().getBase();
  auto &PostDomTree = getAnalysis<MachinePostDominatorTree>().getBase();

  assert(DomTree.dominates(&Preheader, &ExitingBlock));
  assert(PostDomTree.dominates(&ExitingBlock, &Preheader));

  std::set<const MachineBasicBlock *, MIRComp> MBBsForReserv;

  // We need to make reservation for all blocks that are dominated by Preheader
  // and postdominated by Exitinig block
  processDomTree(Preheader, ExitingBlock, DomTree, PostDomTree,
                 std::inserter(MBBsForReserv, MBBsForReserv.end()));
  processDomTree(ExitingBlock, Preheader, PostDomTree, DomTree,
                 std::inserter(MBBsForReserv, MBBsForReserv.end()));

  auto [Counter, Limit] = RootPool.getNAvailableRegisters<2>(
      "for loop latch", RegInfo, RegClass, MBBsForReserv, Filter,
      AccessMaskBit::SRW);

  RootPool.addReserved(Preheader, Counter, AccessMaskBit::W);
  RootPool.addReserved(Preheader, Limit, AccessMaskBit::W);
  auto TrackingMode = SGCtx.hasTrackingMode();
  if (UseStackOpt || TrackingMode) {
    // We still have to reserve counter register even when using the stack.
    // Otherwise, this register might be later reserved for other purposes in
    // flow generator (or any other part of snippy) and we'll corrupt the data
    // stored in it. Reservation can be smaller though - only one block.
    auto ReservationMode = TrackingMode ? AccessMaskBit::RW : AccessMaskBit::W;
    RootPool.addReserved(ExitingBlock, Counter, ReservationMode);
    RootPool.addReserved(ExitingBlock, Limit, ReservationMode);
  } else {
    for (const auto *MBB : MBBsForReserv) {
      RootPool.addReserved(*MBB, Counter, AccessMaskBit::W);
      RootPool.addReserved(*MBB, Limit, AccessMaskBit::W);
    }
  }
  return std::make_pair(Counter, Limit);
}

MachineInstr &LoopLatcher::updateLatchBranch(MachineLoop &ML,
                                             MachineInstr &Branch,
                                             MachineBasicBlock &Preheader,
                                             unsigned CounterReg,
                                             unsigned LimitReg) {
  assert(Branch.isConditionalBranch() && "Conditional branch expected");
  assert(ML.contains(&Branch) && "Expected this loop branch");

  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &BranchDesc = InstrInfo.get(Branch.getOpcode());
  const auto &SnippyTgt = State.getSnippyTarget();

  LLVM_DEBUG(dbgs() << "Old branch: "; Branch.dump());
  auto &NewBranch =
      SnippyTgt.updateLoopBranch(Branch, BranchDesc, CounterReg, LimitReg);
  LLVM_DEBUG(dbgs() << "New branch: "; NewBranch.dump());
  return NewBranch;
}

void LoopLatcher::processExitingBlock(MachineLoop &ML,
                                      MachineBasicBlock &ExitingBlock,
                                      MachineBasicBlock &Preheader) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &State = SGCtx.getLLVMState();
  auto TrackingMode = SGCtx.hasTrackingMode();

  auto FirstTerm = ExitingBlock.getFirstTerminator();
  assert(FirstTerm != ExitingBlock.end() &&
         "ExitingBlock expected to have terminator");
  assert(
      FirstTerm->isConditionalBranch() &&
      "ExitingBlock expected to have conditional branch as first terminator");
  auto &Branch = *FirstTerm;
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &RegInfo = State.getRegInfo();
  const auto &BranchDesc = InstrInfo.get(Branch.getOpcode());
  const auto &MCRegClass = getMCRegClassForBranch(BranchDesc, RegInfo);

  auto [CounterReg, LimitReg] =
      selectRegsForBranch(ML, Preheader, ExitingBlock, MCRegClass);
  LLVM_DEBUG(dbgs() << "Selected regs: " << RegInfo.getRegClassName(&MCRegClass)
                    << ": " << RegInfo.getName(CounterReg) << "[CounterReg] & "
                    << RegInfo.getName(LimitReg) << "[LimitReg]\n");

  auto &NewBranch =
      updateLatchBranch(ML, Branch, Preheader, CounterReg, LimitReg);
  Branch.removeFromParent();

  assert(SGCtx.getConfig().Branches.NLoopIter.Min);
  assert(SGCtx.getConfig().Branches.NLoopIter.Max);
  auto NIterMin = *SGCtx.getConfig().Branches.NLoopIter.Min;
  auto NIterMax = *SGCtx.getConfig().Branches.NLoopIter.Max;
  auto NIter = RandEngine::genInInterval(NIterMin, NIterMax);
  LLVM_DEBUG(dbgs() << "Loop counter init inserting: " << NIter
                    << " iterations, ");
  LLVM_DEBUG(dbgs() << State.getRegInfo().getName(CounterReg) << "("
                    << CounterReg << ")[CounterReg], ");
  LLVM_DEBUG(dbgs() << State.getRegInfo().getName(LimitReg) << "(" << LimitReg
                    << ")[LimitReg]\n");

  auto PreheaderInsertPt = Preheader.getFirstTerminator();
  SnippyTgt.insertLoopInit(Preheader, PreheaderInsertPt, NewBranch, CounterReg,
                           LimitReg, NIter, SGCtx);
  if (UseStackOpt || TrackingMode) {
    SnippyTgt.generateSpill(Preheader, PreheaderInsertPt, CounterReg, SGCtx);
    SnippyTgt.generateSpill(Preheader, PreheaderInsertPt, LimitReg, SGCtx);
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
  if (UseStackOpt || TrackingMode) {
    SnippyTgt.generateReload(*InsMBB, InsPos, LimitReg, SGCtx);
    SnippyTgt.generateReload(*InsMBB, InsPos, CounterReg, SGCtx);
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
      InsPos, NewBranch, CounterReg, LimitReg, NIter, SGCtx, ExitingValues);
  auto &Diag = CounterInsRes.Diag;
  auto ActualNumIter = CounterInsRes.NIter;
  unsigned MinCounterVal = CounterInsRes.MinCounterVal.getZExtValue();
  GeneratorContext::LoopGenerationInfo TheLoopGenInfo{CounterReg, ActualNumIter,
                                                      MinCounterVal};
  SGCtx.addLoopGenerationInfoForMBB(ML.getHeader(), TheLoopGenInfo);

  if (UseStackOpt || TrackingMode) {
    auto *Exit = ML.getExitBlock();
    assert(Exit);
    SGCtx.addIncomingValues(Exit, std::move(ExitingValues));
    SnippyTgt.generateSpill(*InsMBB, InsPos, CounterReg, SGCtx);
    SnippyTgt.generateSpill(*InsMBB, InsPos, LimitReg, SGCtx);
    SnippyTgt.generatePopNoReload(*Exit, Exit->getFirstNonPHI(), LimitReg,
                                  SGCtx);
    SnippyTgt.generatePopNoReload(*Exit, Exit->getFirstNonPHI(), CounterReg,
                                  SGCtx);
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
  auto &MLI = getAnalysis<MachineLoopInfo>();
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
