//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RISCV-specific code-size estimation tests for LLVMState.
///
//===----------------------------------------------------------------------===//

#include "RISCVInstrInfo.h"

#include "snippy/unittests/SnippyState.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Module.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace llvm::snippy {

namespace {

struct LLVMStateTest : public testing::SnippyState {
  LLVMStateTest() : SnippyState("riscv64", "", "", "") {}
};

Module &initModule(Module &M, LLVMState &State) {
  M.setDataLayout(State.getTargetMachine().createDataLayout());
  return M;
}

} // namespace

//===-------------------------- code-size estimation ---------------------===//

namespace {

MachineInstr *appendInstr(const MCInstrDesc &Desc, MachineBasicBlock &MBB,
                          bool DoNotCompress = false) {
  auto MI = BuildMI(MBB, MBB.end(), DebugLoc(), Desc).addReg(RISCV::X1);
  if (DoNotCompress)
    MI->setAsmPrinterFlag(RISCV::DoNotCompress);
  return MI;
}

} // namespace

TEST_F(LLVMStateTest, GetCodeBlockSizeSumsInstrSize) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(RISCV::ADDI);
  const MCInstrDesc &CAddDesc = State.getInstrInfo().get(RISCV::C_ADDI);

  // 4-byte and 2-byte instructions.
  appendInstr(AddDesc, *MBB, /*DoNotCompress=*/true);
  appendInstr(CAddDesc, *MBB, /*DoNotCompress=*/true);
  appendInstr(AddDesc, *MBB, /*DoNotCompress=*/true);

  ASSERT_EQ(MBB->size(), 3u);
  EXPECT_EQ(MBB->front().getOpcode(), unsigned(RISCV::ADDI));
  EXPECT_EQ(MBB->back().getOpcode(), unsigned(RISCV::ADDI));

  size_t Expected = 0;
  for (const MachineInstr &MI : *MBB)
    Expected += State.getSnippyTarget().getInstrSize(MI, State);

  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  // 4 + 2 + 4 bytes.
  EXPECT_EQ(State.getMBBSize(*MBB), 10u);
}

TEST_F(LLVMStateTest, GetFunctionSizeSumsAcrossBlocks) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB1 = MF.CreateMachineBasicBlock();
  MF.push_back(MBB1);
  auto *MBB2 = MF.CreateMachineBasicBlock();
  MF.push_back(MBB2);

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(RISCV::ADDI);
  const MCInstrDesc &CAddDesc = State.getInstrInfo().get(RISCV::C_ADDI);

  // Block 1: two 4-byte instructions.
  appendInstr(AddDesc, *MBB1, /*DoNotCompress=*/true);
  appendInstr(AddDesc, *MBB1, /*DoNotCompress=*/true);
  // Block 2: one 2-byte and one 4-byte instruction.
  appendInstr(CAddDesc, *MBB2, /*DoNotCompress=*/true);
  appendInstr(AddDesc, *MBB2, /*DoNotCompress=*/true);

  EXPECT_EQ(State.getMBBSize(*MBB1), 8u);
  EXPECT_EQ(State.getMBBSize(*MBB2), 6u);
  EXPECT_EQ(State.getFunctionSize(MF), 14u);
}

TEST_F(LLVMStateTest, GetFunctionSizeUnknownSizeAddsZero) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(RISCV::ADDI);
  // A regular 4-byte instruction.
  appendInstr(AddDesc, *MBB, /*DoNotCompress=*/true);
  // A meta-instruction (IMPLICIT_DEF) whose size is unknown (getInstrSize
  // returns 0); it contributes 0 and triggers the InstructionSizeUnknown
  // warning.
  appendInstr(State.getInstrInfo().get(TargetOpcode::IMPLICIT_DEF), *MBB,
              /*DoNotCompress=*/false);

  ASSERT_EQ(MBB->size(), 2u);
  EXPECT_EQ(State.getSnippyTarget().getInstrSize(MBB->front(), State), 4u);
  EXPECT_EQ(State.getSnippyTarget().getInstrSize(MBB->back(), State), 0u);

  // The unknown-size instruction triggers the InstructionSizeUnknown warning on
  // stderr while it contributes 0 to the estimated size.
  ::testing::internal::CaptureStderr();
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), 4u);
  EXPECT_EQ(State.getFunctionSize(MF), 4u);
  std::string Warning = ::testing::internal::GetCapturedStderr();
  EXPECT_THAT(Warning, ::testing::HasSubstr("instruction-size-unknown"));
}

TEST_F(LLVMStateTest, GetCodeBlockSizeAccountsForBranchCallVectorInstructions) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  // A second block used as the branch/call target.
  auto *TargetMBB = MF.CreateMachineBasicBlock();
  MF.push_back(TargetMBB);

  auto appendBare = [&](const MCInstrDesc &Desc, ArrayRef<unsigned> Regs,
                        MachineBasicBlock *Target = nullptr) {
    auto Builder = BuildMI(*MBB, MBB->end(), DebugLoc(), Desc);
    for (unsigned Reg : Regs)
      Builder.addReg(Reg);
    if (Target)
      Builder.addMBB(Target);
    Builder->setAsmPrinterFlag(RISCV::DoNotCompress);
    return Builder;
  };

  const auto &InstrInfo = State.getInstrInfo();
  appendBare(InstrInfo.get(RISCV::ADDI), {RISCV::X1});
  appendBare(InstrInfo.get(RISCV::BEQ), {RISCV::X1, RISCV::X2}, TargetMBB);
  appendBare(InstrInfo.get(RISCV::JAL), {}, TargetMBB);
  appendBare(InstrInfo.get(RISCV::VADD_VV), {RISCV::V0, RISCV::V1, RISCV::V2});

  ASSERT_EQ(MBB->size(), 4u);

  // Every instruction kind must be reported with a positive size; the block
  // size must be the exact sum of the per-instruction sizes.
  size_t Expected = 0;
  for (const MachineInstr &MI : *MBB) {
    unsigned Size = State.getSnippyTarget().getInstrSize(MI, State);
    EXPECT_GT(Size, 0u);
    Expected += Size;
  }
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  EXPECT_EQ(State.getMBBSize(*MBB), Expected);
  // The block containing only the branch/call target stays empty.
  EXPECT_EQ(State.getMBBSize(*TargetMBB), 0u);
  EXPECT_EQ(State.getFunctionSize(MF), Expected);
}

} // namespace llvm::snippy
