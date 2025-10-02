//===-- RISCVExpandSnippyPseudoInsts.cpp -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This file contains a pass that expands snippy specific pseudo instructions
// into target instructions. This pass should be run before other RISC-V
// pseudo expand passes.
//
//===---------------------------------------------------------------------===//

#include "../../InitializePasses.h"
#include "snippy/CreatePasses.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContextPass.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Policy.h"

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "RISCVInstrInfo.h"
#include "RISCVTargetMachine.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define RISCV_EXPAND_SNIPPY_PSEUDO_NAME                                        \
  "RISC-V snippy pseudo instruction expansion pass"

namespace {

class RISCVExpandSnippyPseudo : public MachineFunctionPass {
  const RISCVSubtarget *STI;
  const RISCVInstrInfo *TII;

public:
  static char ID;

  RISCVExpandSnippyPseudo() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return RISCV_EXPAND_SNIPPY_PSEUDO_NAME;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<snippy::GeneratorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  bool expandMBB(MachineBasicBlock &MBB);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
  bool expandAuipcInstPair(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI, unsigned FlagsHi,
                           unsigned SecondOpcode, unsigned DestReg);
  bool expandC_JRB(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
};

char RISCVExpandSnippyPseudo::ID = 0;

bool RISCVExpandSnippyPseudo::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<RISCVSubtarget>();
  TII = STI->getInstrInfo();
  auto &SGCtx = getAnalysis<snippy::GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();

  [[maybe_unused]] const size_t OldSize = State.getFunctionSize(MF);

  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= expandMBB(MBB);

  [[maybe_unused]] const size_t NewSize = State.getFunctionSize(MF);
  assert(OldSize >= NewSize);

  return Modified;
}

bool RISCVExpandSnippyPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  for (auto &&MI : make_early_inc_range(MBB))
    Modified |= expandMI(MBB, MI);

  return Modified;
}

bool RISCVExpandSnippyPseudo::expandMI(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI) {
  switch (MBBI->getOpcode()) {
  case RISCV::PseudoSnippyC_JRB:
    return expandC_JRB(MBB, MBBI);
  }
  return false;
}

bool RISCVExpandSnippyPseudo::expandAuipcInstPair(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI, unsigned FlagsHi,
    unsigned SecondOpcode, unsigned DestReg) {
  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();
  auto &SGCtx = getAnalysis<snippy::GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &Tgt = State.getSnippyTarget();

  MachineOperand &Symbol = MI.getOperand(0);
  Symbol.setTargetFlags(FlagsHi);
  MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("pcrel_hi");

  MachineInstr *MIAUIPC = getSupportInstBuilder(Tgt, MBB, MBBI, State.getCtx(),
                                                TII->get(RISCV::AUIPC), DestReg)
                              .add(Symbol)
                              .getInstr();
  MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

  getSupportInstBuilder(Tgt, MBB, MBBI, State.getCtx(), TII->get(SecondOpcode),
                        DestReg)
      .addReg(DestReg)
      .addSym(AUIPCSymbol, RISCVII::MO_PCREL_LO)
      .getInstr();

  return true;
}

bool RISCVExpandSnippyPseudo::expandC_JRB(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MBBI) {
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();
  auto &SGCtx = getAnalysis<snippy::GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  auto &State = ProgCtx.getLLVMState();
  auto &Tgt = State.getSnippyTarget();

  auto &RI = State.getRegInfo();
  snippy::planning::InstructionGenerationContext IGC{
      MBB, MBB.getFirstTerminator(), SGCtx};
  auto RP = IGC.pushRegPool();
  auto &InstrDesc = TII->get(RISCV::PseudoC_JRB);
  auto &RegClass = RI.getRegClass(InstrDesc.operands()[0].RegClass);
  unsigned DestReg =
      RP->getAvailableRegister("c.jr destination register", RI, RegClass, MBB);

  expandAuipcInstPair(MBB, MBBI, RISCVII::MO_PCREL_HI, RISCV::ADDI, DestReg);
  getMainInstBuilder(Tgt, MBB, MBBI, State.getCtx(), TII->get(RISCV::C_JR),
                     DestReg);
  MI.eraseFromParent();
  return true;
}

} // namespace

INITIALIZE_PASS(RISCVExpandSnippyPseudo, "riscv-expand-snippy-pseudo",
                RISCV_EXPAND_SNIPPY_PSEUDO_NAME, false, false)

namespace llvm {

MachineFunctionPass *createRISCVExpandSnippyPseudoPass() {
  return new RISCVExpandSnippyPseudo();
}

} // namespace llvm