//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// X86-specific code-size estimation tests for LLVMState.
///
//===----------------------------------------------------------------------===//

#include "X86InstrInfo.h"

#include "snippy/unittests/SnippyState.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Module.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <numeric>

namespace llvm::snippy {

namespace {

struct LLVMStateTest : public testing::SnippyState {
  LLVMStateTest() : SnippyState("x86_64", "", "", "") {}
};

Module &initModule(Module &M, LLVMState &State) {
  M.setDataLayout(State.getTargetMachine().createDataLayout());
  return M;
}

} // namespace

//===-------------------------- code-size estimation ---------------------===//

namespace {

MachineInstr *appendMov(MachineBasicBlock &MBB, const MCInstrDesc &Desc,
                        unsigned Dst, unsigned Src) {
  return BuildMI(MBB, MBB.end(), DebugLoc(), Desc).addReg(Dst).addReg(Src);
}

MachineInstr *appendMeta(MachineBasicBlock &MBB, const MCInstrDesc &Desc,
                         unsigned Reg) {
  return BuildMI(MBB, MBB.end(), DebugLoc(), Desc).addReg(Reg);
}

// Compose the expected block size as the sum of the per-instruction sizes
// reported by the target.
size_t getExpectedSize(LLVMState &State, MachineBasicBlock &MBB) {
  return std::accumulate(
      MBB.begin(), MBB.end(), 0u, [&State](size_t Acc, const MachineInstr &MI) {
        return Acc + State.getSnippyTarget().getInstrSize(MI, State);
      });
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

  const MCInstrDesc &MovDesc = State.getInstrInfo().get(X86::MOV64rr);

  appendMov(*MBB, MovDesc, X86::RAX, X86::RBX);
  appendMov(*MBB, MovDesc, X86::RCX, X86::RDX);
  appendMov(*MBB, MovDesc, X86::RSI, X86::RDI);

  ASSERT_EQ(MBB->size(), 3u);
  EXPECT_EQ(MBB->front().getOpcode(), unsigned(X86::MOV64rr));
  EXPECT_EQ(MBB->back().getOpcode(), unsigned(X86::MOV64rr));

  size_t Expected = getExpectedSize(State, *MBB);
  EXPECT_GT(Expected, 0u);
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  EXPECT_EQ(State.getMBBSize(*MBB), Expected);
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

  const MCInstrDesc &MovDesc = State.getInstrInfo().get(X86::MOV64rr);

  // Block 1: two instructions.
  appendMov(*MBB1, MovDesc, X86::RAX, X86::RBX);
  appendMov(*MBB1, MovDesc, X86::RCX, X86::RDX);
  // Block 2: two instructions.
  appendMov(*MBB2, MovDesc, X86::RSI, X86::RDI);
  appendMov(*MBB2, MovDesc, X86::R8, X86::R9);

  size_t Block1 = getExpectedSize(State, *MBB1);
  size_t Block2 = getExpectedSize(State, *MBB2);
  EXPECT_GT(Block1, 0u);
  EXPECT_GT(Block2, 0u);
  EXPECT_EQ(State.getMBBSize(*MBB1), Block1);
  EXPECT_EQ(State.getMBBSize(*MBB2), Block2);
  EXPECT_EQ(State.getFunctionSize(MF), Block1 + Block2);
}

TEST_F(LLVMStateTest, GetFunctionSizeSumsEveryInstr) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());
  auto &MF = State.createMachineFunction(
      M, MMI, "func", "", Function::ExternalLinkage, State.getCtx());

  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);

  const MCInstrDesc &MovDesc = State.getInstrInfo().get(X86::MOV64rr);
  // A regular instruction.
  appendMov(*MBB, MovDesc, X86::RAX, X86::RBX);
  // A meta-instruction (IMPLICIT_DEF); whatever size the target reports for it,
  // it must be accounted for by the block/function size accumulator.
  auto *MetaMI = appendMeta(
      *MBB, State.getInstrInfo().get(TargetOpcode::IMPLICIT_DEF), X86::RAX);

  ASSERT_EQ(MBB->size(), 2u);
  EXPECT_EQ(MBB->back().getOpcode(), unsigned(TargetOpcode::IMPLICIT_DEF));
  EXPECT_EQ(MetaMI->getOpcode(), unsigned(TargetOpcode::IMPLICIT_DEF));

  size_t Expected = getExpectedSize(State, *MBB);
  EXPECT_GT(Expected, 0u);
  // The X86 getInstrSize() heuristic never reports an unknown (zero) size, so
  // unlike RISCV no InstructionSizeUnknown warning is expected on stderr.
  ::testing::internal::CaptureStderr();
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  EXPECT_EQ(State.getFunctionSize(MF), Expected);
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

  const auto &InstrInfo = State.getInstrInfo();
  // A scalar instruction.
  appendMov(*MBB, InstrInfo.get(X86::MOV64rr), X86::RAX, X86::RBX);
  // A relative jump (fixed-size short jump).
  BuildMI(*MBB, MBB->end(), DebugLoc(), InstrInfo.get(X86::JMP_1)).addImm(0);
  // An indirect function call through a 64-bit GPR.
  BuildMI(*MBB, MBB->end(), DebugLoc(), InstrInfo.get(X86::CALL64r))
      .addReg(X86::RAX);
  // A vector register move.
  appendMov(*MBB, InstrInfo.get(X86::MOVAPSrr), X86::XMM0, X86::XMM1);

  ASSERT_EQ(MBB->size(), 4u);

  // Branch, call and vector opcodes must all be accounted for: the block and
  // function sizes are the exact sum of the per-instruction sizes regardless
  // of the target's per-instruction heuristic.
  size_t Expected = getExpectedSize(State, *MBB);
  EXPECT_GT(Expected, 0u);
  EXPECT_EQ(State.getCodeBlockSize(MBB->begin(), MBB->end()), Expected);
  EXPECT_EQ(State.getMBBSize(*MBB), Expected);
  EXPECT_EQ(State.getFunctionSize(MF), Expected);
}

} // namespace llvm::snippy
