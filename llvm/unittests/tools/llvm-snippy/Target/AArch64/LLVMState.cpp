//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64-specific code-size estimation tests for LLVMState.
///
//===----------------------------------------------------------------------===//

#include "AArch64InstrInfo.h"

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
  LLVMStateTest() : SnippyState("aarch64", "", "", "") {}
};

Module &initModule(Module &M, LLVMState &State) {
  M.setDataLayout(State.getTargetMachine().createDataLayout());
  return M;
}

} // namespace

//===-------------------------- code-size estimation ---------------------===//

namespace {

MachineInstr *appendInstr(const MCInstrDesc &Desc, MachineBasicBlock &MBB,
                          ArrayRef<unsigned> Regs) {
  auto MI = BuildMI(MBB, MBB.end(), DebugLoc(), Desc);
  for (unsigned Reg : Regs)
    MI.addReg(Reg);
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

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(AArch64::ADDXrr);

  // AArch64 instructions are always 4 bytes.
  appendInstr(AddDesc, *MBB, {AArch64::X0, AArch64::X1, AArch64::X2});
  appendInstr(AddDesc, *MBB, {AArch64::X3, AArch64::X4, AArch64::X5});
  appendInstr(AddDesc, *MBB, {AArch64::X6, AArch64::X7, AArch64::X8});

  ASSERT_EQ(MBB->size(), 3u);
  EXPECT_EQ(MBB->front().getOpcode(), unsigned(AArch64::ADDXrr));
  EXPECT_EQ(MBB->back().getOpcode(), unsigned(AArch64::ADDXrr));

  size_t Expected = 0;
  for (const MachineInstr &MI : *MBB)
    Expected += State.getSnippyTarget().getInstrSize(MI, State);

  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  // 4 + 4 + 4 bytes.
  EXPECT_EQ(State.getMBBSize(*MBB), 12u);
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

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(AArch64::ADDXrr);

  // Block 1: two 4-byte instructions.
  appendInstr(AddDesc, *MBB1, {AArch64::X0, AArch64::X1, AArch64::X2});
  appendInstr(AddDesc, *MBB1, {AArch64::X3, AArch64::X4, AArch64::X5});
  // Block 2: two 4-byte instructions.
  appendInstr(AddDesc, *MBB2, {AArch64::X6, AArch64::X7, AArch64::X8});
  appendInstr(AddDesc, *MBB2, {AArch64::X9, AArch64::X10, AArch64::X11});

  EXPECT_EQ(State.getMBBSize(*MBB1), 8u);
  EXPECT_EQ(State.getMBBSize(*MBB2), 8u);
  EXPECT_EQ(State.getFunctionSize(MF), 16u);
}

TEST_F(LLVMStateTest, GetFunctionSizeSumsEveryInstr) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);

  const MCInstrDesc &AddDesc = State.getInstrInfo().get(AArch64::ADDXrr);
  // A regular 4-byte instruction.
  appendInstr(AddDesc, *MBB, {AArch64::X0, AArch64::X1, AArch64::X2});
  // A meta-instruction (IMPLICIT_DEF) whose size is the target's instruction
  // size; on AArch64 it still accounts for 4 bytes of the total.
  auto *MetaMI = appendInstr(
      State.getInstrInfo().get(TargetOpcode::IMPLICIT_DEF), *MBB, {});

  ASSERT_EQ(MBB->size(), 2u);
  EXPECT_EQ(State.getSnippyTarget().getInstrSize(MBB->front(), State), 4u);
  EXPECT_EQ(State.getSnippyTarget().getInstrSize(MBB->back(), State), 4u);
  EXPECT_EQ(MetaMI->getOpcode(), unsigned(TargetOpcode::IMPLICIT_DEF));

  // AArch64 getInstrSize() always reports a fixed 4-byte size, so unlike RISCV
  // no InstructionSizeUnknown warning is expected on stderr.
  ::testing::internal::CaptureStderr();
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), 8u);
  EXPECT_EQ(State.getFunctionSize(MF), 8u);
  std::string Warning = ::testing::internal::GetCapturedStderr();
  EXPECT_THAT(Warning,
              ::testing::Not(::testing::HasSubstr("instruction-size-unknown")));
}

TEST_F(LLVMStateTest, GetCodeBlockSizeAccountsForBranchCallVectorInstructions) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  auto *TargetMBB = MF.CreateMachineBasicBlock();
  MF.push_back(TargetMBB);

  // Scalar, branch, function-call and vector opcodes have to be accounted for
  // by the code-size estimation. AArch64 instructions are all fixed 4 bytes,
  // and the branch/call keep an explicit MBB target operand.
  const auto &InstrInfo = State.getInstrInfo();
  appendInstr(InstrInfo.get(AArch64::ADDXrr), *MBB,
              {AArch64::X0, AArch64::X1, AArch64::X2});
  BuildMI(*MBB, MBB->end(), DebugLoc(), InstrInfo.get(AArch64::B))
      .addMBB(TargetMBB);
  BuildMI(*MBB, MBB->end(), DebugLoc(), InstrInfo.get(AArch64::BL))
      .addMBB(TargetMBB);
  appendInstr(InstrInfo.get(AArch64::ADDv4i32), *MBB,
              {AArch64::Q0, AArch64::Q1, AArch64::Q2});

  ASSERT_EQ(MBB->size(), 4u);
  EXPECT_EQ(State.getSnippyTarget().getInstrSize(MBB->front(), State), 4u);

  // Block and function sizes must be the exact sum of the per-instruction
  // sizes (each 4 bytes: 4 scalar/branch/call/vector instructions).
  size_t Expected = 0;
  for (const MachineInstr &MI : *MBB) {
    unsigned Size = State.getSnippyTarget().getInstrSize(MI, State);
    EXPECT_EQ(Size, 4u);
    Expected += Size;
  }
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  EXPECT_EQ(State.getMBBSize(*MBB), Expected);
  EXPECT_EQ(State.getMBBSize(*TargetMBB), 0u);
  EXPECT_EQ(State.getFunctionSize(MF), Expected);
}

} // namespace llvm::snippy