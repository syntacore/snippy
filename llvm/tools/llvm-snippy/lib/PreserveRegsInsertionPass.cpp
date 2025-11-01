//=== PreserveRegsInsertionPass.cpp -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <llvm/CodeGen/LivePhysRegs.h>

#include "InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/RootRegPoolWrapperPass.h"
#include "snippy/Generator/TrackLivenessPass.h"

#include <vector>

namespace llvm {
namespace snippy {
namespace {

#define DEBUG_TYPE "snippy-preserve-regs-insertion"
#define PASS_DESC "Snippy Preserve Regs Insertion"

// This pass preserves necessary registers when calling external functions, if
// necessary. Namely:
// 1) global registers
// 2) caller-saved registers
// 3) ensures that the target stack pointer holds the actual stack value.

// If compiled stack optimization is applicable, this pass will also spill the
// caller-saved registers before the call and reload them after the call.

class PreserveRegsInsertion : public MachineFunctionPass {
  using MBBIterTy = MachineBasicBlock::iterator;

public:
  static char ID;

  PreserveRegsInsertion() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    bool WasModifiedExternal = false, WasModifiedInternal = false;
    for (auto &&MBB : MF) {
      preserveRegistersAroundExternalCallInstrs(MBB, WasModifiedExternal);
      preserveRegistersAroundInternalCallInstrs(MBB, WasModifiedInternal);
    }
    return WasModifiedExternal | WasModifiedInternal;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<RootRegPoolWrapper>();
    AU.addRequired<TrackLiveness>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  void reloadGlobalRegsFromMemory(InstructionGenerationContext &IGC) {
    auto &ProgCtx = IGC.ProgCtx;
    auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
    auto &ProgCfg = IGC.getCommonCfg().ProgramCfg;
    auto &SpilledToMem = ProgCfg.SpilledToMem;
    if (SpilledToMem.empty())
      return;
    assert(ProgCtx.hasProgramStateSaveSpace());
    auto &SaveLocs = ProgCtx.getProgramStateSaveSpace();
    for (auto &Reg : SpilledToMem) {
      auto &Addr = SaveLocs.getSaveLocation(Reg);
      Tgt.generateSpillToAddr(IGC, Reg, Addr.Local);
    }
    for (auto &Reg : SpilledToMem) {
      auto &Addr = SaveLocs.getSaveLocation(Reg);
      Tgt.generateReloadFromAddr(IGC, Reg, Addr.Global);
    }
  }

  void reloadLocallySpilledRegs(InstructionGenerationContext &IGC) {
    auto &ProgCtx = IGC.ProgCtx;
    auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
    auto &ProgCfg = IGC.getCommonCfg().ProgramCfg;
    auto &SpilledToMem = ProgCfg.SpilledToMem;
    if (SpilledToMem.empty())
      return;
    assert(ProgCtx.hasProgramStateSaveSpace());
    auto &SaveLocs = ProgCtx.getProgramStateSaveSpace();
    for (auto &Reg : SpilledToMem) {
      auto &Addr = SaveLocs.getSaveLocation(Reg);
      Tgt.generateReloadFromAddr(IGC, Reg, Addr.Local);
    }
  }

  auto getPreserveRegs(MachineFunction &MF, const SnippyTarget &SnippyTgt) {
    auto PreserveRegs =
        SnippyTgt.getCallerSavedRegs(MF, SnippyTgt.getCallerSavedRegGroups());
    auto MutatedRegs = getAllMutatedRegs(MF);
    // The remaining mutated registers were saved in the function's prologue.
    // Works only with sorted containers.
    llvm::sort(PreserveRegs);
    std::vector<MCRegister> Result;
    std::set_intersection(PreserveRegs.begin(), PreserveRegs.end(),
                          MutatedRegs.begin(), MutatedRegs.end(),
                          std::back_inserter(Result));
    auto RAIt = llvm::find(Result, SnippyTgt.getReturnAddress());
    if (RAIt != Result.end())
      Result.erase(RAIt);
    return Result;
  }

  void preserveRegistersAroundInternalCallInstrs(MachineBasicBlock &MBB,
                                                 bool &WasModified) {
    auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
    // When we do not have compiled stack, we spill all registers in prologue.
    if (!SGCtx.getProgramContext().getConfig().StaticStack)
      return;
    auto &ProgCtx = SGCtx.getProgramContext();
    auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
    auto &MF = *MBB.getParent();
    auto PreserveRegs = getPreserveRegs(MF, SnippyTgt);

    auto *TRI = MF.getSubtarget().getRegisterInfo();
    assert(TRI && "register information must be available");

    // Get only internal call instrs.
    auto InternalCalls = llvm::make_filter_range(MBB, [&](auto &&Instr) {
      return Instr.isCall() &&
             !checkMetadata(Instr, SnippyMetadata::ExternalCall);
    });
    auto RPW = getAnalysis<RootRegPoolWrapper>().getPool();
    auto &StaticStackCtx = ProgCtx.getStaticStack();
    StaticStackCtx.reset();
    StaticStackCtx.setSPAddrLocal(StaticStackCtx.getSPAddrGlobal());
    for (auto &&CallInstr : InternalCalls) {
      auto CallIt = CallInstr.getIterator();
      auto InsertTo = CallIt;
      if (CallIt != MBB.begin()) {
        // We must insert spills before the address is formed, so as not to
        // overwrite the register with the address.
        InsertTo =
            llvm::find_if_not(make_range((--CallIt)->getReverseIterator(),
                                         MBB.begin()->getReverseIterator()),
                              [](const auto &Instr) {
                                return checkMetadata(
                                    Instr, SnippyMetadata::FormAddrForCall);
                              })
                ->getIterator();
        if (!checkMetadata(*InsertTo, SnippyMetadata::FormAddrForCall))
          ++InsertTo;
        ++CallIt;
      }

      std::vector<MCRegister> RegsToSpill = [&] {
        if (MF.getProperties().hasProperty(
                MachineFunctionProperties::Property::TracksLiveness))
          return getLiveInPreservedRegsForCall(PreserveRegs, MBB, CallInstr,
                                               SnippyTgt, ProgCtx, *TRI);
        return PreserveRegs;
      }();

      // We must reserve ALL registers first, so as not to overwrite any of them
      // in the process of forming an address for spilling another register.
      for (auto SpillReg : RegsToSpill)
        RPW.addReserved(SpillReg);
      auto RealStackPointer = ProgCtx.getStackPointer();
      InstructionGenerationContext InstrGenCtxToSpill{MBB, *InsertTo, SGCtx,
                                                      RPW};
      // Spill caller saved registers to stack.
      for (auto &&Reg : RegsToSpill)
        SnippyTgt.generateSpillToStack(InstrGenCtxToSpill, Reg,
                                       RealStackPointer);
      StaticStackCtx.passSPAddr();

      // Go through call instruction.
      assert(CallIt->isCall());
      ++CallIt;
      InstructionGenerationContext InstrGenCtxToReload{MBB, CallIt, SGCtx, RPW};
      // Reload caller saved registers from stack.
      for (auto &&Reg : llvm::reverse(RegsToSpill))
        SnippyTgt.generateReloadFromStack(InstrGenCtxToReload, Reg,
                                          RealStackPointer);
      StaticStackCtx.resetRegWithSPAddrLocal();

      WasModified |= !RegsToSpill.empty();
    }
  }

  // -- Algorithm for spilling and reloading caller-saved registers before a
  // call to an external function:
  // 1) For each MBB, obtain the set of call instructions (those that
  // call external functions).
  // 2) For each call instruction:
  // -- Obtain all registers defined (defs) from the beginning of the MBB up
  //    to the call instruction.
  // -- From the call instruction to the end of the MBB, obtain all registers
  //    used (uses) that occur before their first definition (def).
  // -- Intersect the two sets of registers obtained and
  //    add the resulting set to the live-in registers for the MBB.
  // 3) Preserve only the registers that must be saved according to the
  // ABI.
  void preserveRegistersAroundExternalCallInstrs(MachineBasicBlock &MBB,
                                                 bool &WasModified) {
    auto RPW = getAnalysis<RootRegPoolWrapper>().getPool();
    auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
    auto &ProgCfg = SGCtx.getConfig().ProgramCfg;
    auto &ProgCtx = SGCtx.getProgramContext();
    auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
    auto TargetStackPointer = SnippyTgt.getStackPointer();
    auto RealStackPointer = ProgCtx.getStackPointer();
    auto &RequestedCallerSavedGroups = ProgCtx.preserveCallerSavedGroups();

    auto &MF = *MBB.getParent();
    auto RequestedPreserveRegs =
        SnippyTgt.getCallerSavedRegs(MF, RequestedCallerSavedGroups);
    auto ShouldPreserveCallerRegs =
        ProgCfg->FollowTargetABI || !RequestedPreserveRegs.empty();

    auto *TRI = MF.getSubtarget().getRegisterInfo();
    assert(TRI && "register information must be available");

    auto AllCallerRegsToPreserve = SnippyTgt.getCallerSavedRegs(
        MF, SnippyTgt.getCallerSavedLiveRegGroups());
    // Get only external call instrs.
    auto ExternalCalls = llvm::make_filter_range(MBB, [&](auto &&Instr) {
      return checkMetadata(Instr, SnippyMetadata::ExternalCall);
    });
    for (auto &&CallInstr : ExternalCalls) {
      assert(CallInstr.isCall());
      InstructionGenerationContext InstrGenCtx{MBB, CallInstr, SGCtx, RPW};
      std::vector<MCRegister> RegsToSpill;
      if (!RequestedPreserveRegs.empty())
        RegsToSpill = RequestedPreserveRegs;
      else if (ShouldPreserveCallerRegs) {
        if (MF.getProperties().hasProperty(
                MachineFunctionProperties::Property::TracksLiveness))
          RegsToSpill = getLiveInPreservedRegsForCall(AllCallerRegsToPreserve,
                                                      MBB, CallInstr, SnippyTgt,
                                                      ProgCtx, *TRI);
        else
          RegsToSpill = AllCallerRegsToPreserve;
      }
      // Spill caller saved registers to the stack.
      if (ShouldPreserveCallerRegs)
        for (auto &&Reg : RegsToSpill)
          SnippyTgt.generateSpillToStack(InstrGenCtx, Reg, RealStackPointer);
      if (ProgCfg->hasSectionToSpillGlobalRegs())
        reloadGlobalRegsFromMemory(InstrGenCtx);
      // If we redefined stack pointer register, before generating external
      // function call we need to copy stack pointer value to target default
      // stack pointer and do reverse after returning from external call.
      if (RealStackPointer != TargetStackPointer)
        SnippyTgt.copyRegToReg(InstrGenCtx, RealStackPointer,
                               TargetStackPointer);
      // Go through the call instruction.
      InstrGenCtx.Ins++;

      if (!ProgCfg->isRegSpilledToMem(RealStackPointer) &&
          (RealStackPointer != TargetStackPointer))
        SnippyTgt.copyRegToReg(InstrGenCtx, TargetStackPointer,
                               RealStackPointer);
      if (ProgCfg->hasSectionToSpillGlobalRegs())
        reloadLocallySpilledRegs(InstrGenCtx);
      // Reload caller saved registers from the stack.
      if (ShouldPreserveCallerRegs)
        for (auto &&Reg : llvm::reverse(RegsToSpill))
          SnippyTgt.generateReloadFromStack(InstrGenCtx, Reg, RealStackPointer);

      WasModified |= !RegsToSpill.empty();
    }
  }

  // Collect all defs from MBB.begin() to CallInstrIter.
  std::set<MCRegister> getDefsBeforeCallInstr(MBBIterTy MBBBegin,
                                              MBBIterTy CallInstrIter) const {
    std::set<MCRegister> Defs;
    for (auto &&Instr : llvm::make_range(MBBBegin, CallInstrIter)) {
      auto RegDefs = llvm::make_filter_range(
          Instr.defs(), [&](auto &&DefOper) { return DefOper.isReg(); });
      llvm::transform(RegDefs, std::inserter(Defs, Defs.end()),
                      [](auto &&DefOper) {
                        assert(DefOper.isReg());
                        return DefOper.getReg();
                      });
    }
    return Defs;
  }

  // Obtain the set of registers that are used after the call and before their
  // first redefinition.
  std::set<MCRegister>
  getUsesBeforeFirstDefAfterCallInstr(MBBIterTy CallInstrIter,
                                      MBBIterTy MBBEnd) const {
    DenseSet<MachineOperand> Defs;
    std::set<MCRegister> RegUses;
    for (auto &&Instr : llvm::make_range(CallInstrIter, MBBEnd)) {
      auto NotDeadUses =
          llvm::make_filter_range(Instr.uses(), [&](auto &&UseOper) {
            return UseOper.isReg() && !Defs.count(UseOper);
          });
      llvm::transform(NotDeadUses, std::inserter(RegUses, RegUses.end()),
                      [](auto &&UseOper) {
                        assert(UseOper.isReg());
                        return UseOper.getReg();
                      });
      Defs.insert(Instr.defs().begin(), Instr.defs().end());
    }
    return RegUses;
  }

  std::vector<MCRegister>
  getLiveInPreservedRegsForCall(std::vector<MCRegister> AllCallerRegsToPreserve,
                                MachineBasicBlock &MBB, MachineInstr &CallInstr,
                                const SnippyTarget &SnippyTgt,
                                const SnippyProgramContext &ProgCtx,
                                const TargetRegisterInfo &TRI) const {
    if (AllCallerRegsToPreserve.empty())
      return {};
    LivePhysRegs LiveRegs(TRI);
    LiveRegs.addLiveIns(MBB);
    std::set<MCRegister> LiveRegsBeforeCall(LiveRegs.begin(), LiveRegs.end());

    auto DefsBeforeCall =
        getDefsBeforeCallInstr(MBB.begin(), MBBIterTy(CallInstr));
    auto UsesAfterCall =
        getUsesBeforeFirstDefAfterCallInstr(MBBIterTy(CallInstr), MBB.end());
    auto LiveRegsAroundCall =
        llvm::set_intersection(DefsBeforeCall, UsesAfterCall);

    LiveRegsBeforeCall.insert(LiveRegsAroundCall.begin(),
                              LiveRegsAroundCall.end());
    // Preserve only most high registers.
    for (auto &&CallerReg : AllCallerRegsToPreserve)
      for (auto &&Subreg : TRI.subregs(CallerReg))
        if (LiveRegsBeforeCall.count(Subreg)) {
          LiveRegsBeforeCall.erase(Subreg);
          LiveRegsBeforeCall.insert(CallerReg);
        }
    // std::set_intersection requires sorted ranges.
    llvm::sort(AllCallerRegsToPreserve);
    std::vector<MCRegister> RegsToSpill;
    // Collect live-in caller-saved registers.
    std::set_intersection(AllCallerRegsToPreserve.begin(),
                          AllCallerRegsToPreserve.end(),
                          LiveRegsBeforeCall.begin(), LiveRegsBeforeCall.end(),
                          std::back_inserter(RegsToSpill));
    return RegsToSpill;
  }
};

char PreserveRegsInsertion::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::PreserveRegsInsertion;

INITIALIZE_PASS(PreserveRegsInsertion, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createPreserveRegsInsertionPass() {
  return new snippy::PreserveRegsInsertion;
}

} // namespace llvm
