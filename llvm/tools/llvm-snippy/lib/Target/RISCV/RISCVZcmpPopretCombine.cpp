//===-- RISCVZcmpPopretCombine.cpp -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This file contains a pass that identifies and matches destroy stack frame
// (load ra and 0 to 12 saved registers from the stack frame, deallocate the
// stack frame, return to ra) to cm.popret intruction from the Zcmp extension.
//
//===---------------------------------------------------------------------===//

#include "../../InitializePasses.h"
#include "RISCVGenerated.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/Policy.h"
#include "snippy/GeneratorUtils/LLVMState.h"

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;
using namespace snippy;

#define RISCV_ZCMP_POPRET_COMBINE_NAME "RISC-V zcmp popret combine pass"

namespace {

class RISCVZcmpPopretCombine : public MachineFunctionPass {
  const RISCVSubtarget *STI;
  OpcGenHolder PopretGen;

public:
  static char ID;

  RISCVZcmpPopretCombine() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return RISCV_ZCMP_POPRET_COMBINE_NAME;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<FunctionGenerator>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  void replaceEpilogueWithPopret(unsigned PopretOpcode, uint64_t RList,
                                 MachineBasicBlock &MBB);
  // Returns RList value for CM_POPRET/CM_POPRETZ instruction
  uint64_t replacePrologueWithRListSpill(unsigned PopretOpcode,
                                         MachineBasicBlock &MBB);
};

char RISCVZcmpPopretCombine::ID = 0;

bool RISCVZcmpPopretCombine::runOnMachineFunction(MachineFunction &MF) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  const auto &Hist = SGCtx.getConfig().Histogram;
  if (!Hist.contains(RISCV::CM_POPRET) && !Hist.contains(RISCV::CM_POPRETZ))
    return false;

  auto &FG = getAnalysis<FunctionGenerator>();
  // We can't replace epilogue in entry function, because it doesn't always
  // end with RET instruction.
  if (FG.isEntryFunction(MF))
    return false;

  if (!PopretGen) {
    STI = &MF.getSubtarget<RISCVSubtarget>();
    // We add weight of the PreudoRET to indicate the case when we do
    // not replace the usual prologue and epilogue with any popret instruction.
    std::map<unsigned, double> PopretOpcWeight = {
        {RISCV::PseudoRET, 1.0},
        {RISCV::CM_POPRET, Hist.weight(RISCV::CM_POPRET)},
        {RISCV::CM_POPRETZ, Hist.weight(RISCV::CM_POPRETZ)}};
    OpcodeHistogram PopretHist(PopretOpcWeight);
    PopretGen = std::make_unique<DefaultOpcodeGenerator>(PopretHist);
  }

  assert(!MF.empty());
  // Prologue spills only those callee-saved registers that change in the
  // function. Instructions CM_POPRET/CM_POPRETZ reloads registers depending on
  // its argument RList. To cover all possible values of the instruction
  // arguments, we need to replace the prologue with a spill of exactly as many
  // registers as CM_POPRET/CM_POPRETZ will reload.
  auto PopretOpcode = generateSingleOpcode(*PopretGen);
  if (PopretOpcode == RISCV::PseudoRET)
    return false;
  if (SGCtx.getProgramContext().getReturnAddress() != RISCV::X1) {
    snippy::fatal("Cannot generate cm.popret(z) with non-abi return adress "
                  "register. Please, use redefine-ra=RA");
  }
  auto RList = replacePrologueWithRListSpill(PopretOpcode, MF.front());
  replaceEpilogueWithPopret(PopretOpcode, RList, MF.back());
  return true;
}

// Returns RList value for CM_POPRET/CM_POPRETZ instruction
uint64_t
RISCVZcmpPopretCombine::replacePrologueWithRListSpill(unsigned PopretOpcode,
                                                      MachineBasicBlock &MBB) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();

  // The reverse order, because the corresponding SP decrement occurs before the
  // main store instruction, and we must be able to meet the store first, and
  // then delete the corresponding decrement.
  bool NeedDeleteSPDec = false;
  for (auto &&MI : make_early_inc_range(llvm::reverse(MBB))) {
    if (NeedDeleteSPDec) {
      assert(MI.getOpcode() == RISCV::ADDI);
      MI.eraseFromParent();
      NeedDeleteSPDec = false;
      continue;
    }
    if (!checkMetadata(MI, SnippyMetadata::Prologue) ||
        !isStore(MI.getOpcode()))
      continue;
    const auto &FirstOperand = MI.getOperand(0);
    assert(FirstOperand.isReg());
    // We need delete only GPRs spills, because popret instructions reloads only
    // callee-saved GPRs.
    if (!RISCV::GPRRegClass.contains(FirstOperand.getReg()))
      continue;
    MI.eraseFromParent();
    NeedDeleteSPDec = true;
  }

  assert(!MBB.empty());
  InstructionGenerationContext IGC(MBB, MBB.begin(), SGCtx);
  const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
  const auto &InstrDesc = InstrInfo.get(RISCV::CM_POPRET);
  auto RList = Tgt.generateTargetOperand(InstrDesc, /* OperandIdx */ 0,
                                         /* StridedImm */ {}, ProgCtx,
                                         IGC.getCommonCfg())
                   .getImm();
  // We have already removed creation stack frame (spilling callee-saved
  // registers to the stack). Now we need to spill of exactly as many
  // registers as CM_POPRET/CM_POPRETZ will reload (RList).
  auto SpilledToStack = getSpilledRegsFromRList(RList);
  auto RP = IGC.pushRegPool();
  // Forbid spilled registers to be potentially used as scratch registers
  // for address forming.
  for (auto SpillReg : SpilledToStack)
    RP->addReserved(SpillReg);

  // reverse needed because of the order in which the registers are reloaded in
  // popret instructions
  for (auto SpillReg : llvm::reverse(SpilledToStack)) {
    MBB.addLiveIn(SpillReg);
    Tgt.generateSpillToStack(IGC, SpillReg, RISCV::X2);
  }
  return RList;
}

static auto getRListOffset(uint64_t RList, const LLVMState &State,
                           InstructionGenerationContext &IGC) {
  auto &Tgt = State.getSnippyTarget();
  auto SpillAlignment =
      Tgt.getSpillAlignmentInBytes(/*any GPR*/ RISCV::X0, State);
  auto RegSize =
      Tgt.getRegBitWidth(/*any GPR*/ RISCV::X0, IGC) / RISCV_CHAR_BIT;
  // The magic number 3 is associated with the encoding of the operand RList.
  // The available values start with 4, which means "ra", then 5 - "ra, s0", 6 -
  // "ra, s0-s1", and incrementally.
  assert(RList > 3 && RList < 16 && "Unexpected RList value");
  auto NumRegs = RList - 3;
  auto NumWholeRegsInSlot = SpillAlignment / RegSize;
  // 15 - "ra, s0-s11" needs separate processing.
  if (RList == RISCVZC::RA_S0_S11)
    NumRegs = 13;
  // The number of registers that are missing so that the stack top is aligned
  // to SpillAlignment bytes.
  auto MissingRegs =
      (NumWholeRegsInSlot - NumRegs % NumWholeRegsInSlot) % NumWholeRegsInSlot;
  return MissingRegs * RegSize;
}

void RISCVZcmpPopretCombine::replaceEpilogueWithPopret(unsigned PopretOpcode,
                                                       uint64_t RList,
                                                       MachineBasicBlock &MBB) {
  bool NeedDeleteSPInc = false;
  for (auto &&MI : make_early_inc_range(MBB)) {
    if (NeedDeleteSPInc) {
      assert(MI.getOpcode() == RISCV::ADDI);
      MI.eraseFromParent();
      NeedDeleteSPInc = false;
      continue;
    }
    if (!checkMetadata(MI, SnippyMetadata::Epilogue) || !isLoad(MI.getOpcode()))
      continue;
    const auto &FirstOperand = MI.getOperand(0);
    assert(FirstOperand.isReg());
    // We need delete only GPRs reloads, because popret instructions reloads
    // only callee-saved GPRs.
    if (!RISCV::GPRRegClass.contains(FirstOperand.getReg()))
      continue;
    MI.eraseFromParent();
    NeedDeleteSPInc = true;
  }

  // We have already removed stack termination (reloading of callee-saved
  // registers from the stack). Now we need to delete the RET instruction and
  // insert CM_POPRET/CM_POPRETZ instead of the entire deleted epilogue.
  assert(!MBB.empty());
  MachineInstr &RetInstr = MBB.back();
  assert(RetInstr.isReturn());
  RetInstr.eraseFromParent();
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &Tgt = State.getSnippyTarget();
  const auto *InstrInfo = STI->getInstrInfo();
  const auto &InstrDesc = InstrInfo->get(PopretOpcode);

  InstructionGenerationContext IGC(MBB, MBB.begin(), SGCtx);
  auto Spimm = Tgt.generateTargetOperand(InstrDesc, /* OperandIdx */ 1,
                                         /* StridedImm */ {}, ProgCtx,
                                         IGC.getCommonCfg())
                   .getImm();
  auto &Ctx = State.getCtx();
  // Now SP is not aligned to 16 bytes. This means that we need to subtract
  // Offset(RList) bytes to restore alignment, since CM_POPRET/CM_POPRETZ
  // instruction relies on alignment.
  auto Offset = getRListOffset(RList, State, IGC);
  // spimm is the number of additional 16-byte address increments allocated for
  // the stack frame. This means that CM_POPRET/CM_POPRETZ instruction adds
  // Spimm (= spimm * 16) bytes to the SP, meaning that in order to restore the
  // registers correctly, we must subtract this value from SP.
  getSupportInstBuilder(Tgt, MBB, MBB.end(), Ctx, InstrInfo->get(RISCV::ADDI))
      .addDef(RISCV::X2)
      .addReg(RISCV::X2)
      .addImm(-Spimm - Offset)
      .getInstr();
  getMainInstBuilder(Tgt, MBB, MBB.end(), Ctx, InstrInfo->get(PopretOpcode))
      .addImm(RList)
      .addImm(Spimm)
      .getInstr();
}

} // namespace

INITIALIZE_PASS(RISCVZcmpPopretCombine, "riscv-zcmp-popret-combine",
                RISCV_ZCMP_POPRET_COMBINE_NAME, false, false)

namespace llvm {

MachineFunctionPass *createRISCVZcmpPopretCombinePass() {
  return new RISCVZcmpPopretCombine();
}

} // namespace llvm
