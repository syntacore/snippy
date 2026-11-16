//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Target-independent unit-tests for the LLVMState create* members.
///
/// These tests exercise generic LLVM plumbing (Function/MachineFunction/global
/// constant)
///
//===----------------------------------------------------------------------===//

#include "snippy/unittests/SnippyState.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"

#include "gtest/gtest.h"

namespace llvm::snippy {

namespace {

// The triple comes from the parameterized backend (see Instantiate at the
// bottom of this file).
struct LLVMStateTest : public testing::SnippyState {};

Module &initModule(Module &M, LLVMState &State) {
  M.setDataLayout(State.getTargetMachine().createDataLayout());
  return M;
}

} // namespace

//===-------------------------- createFunction ---------------------------===//

TEST_P(LLVMStateTest, CreateFunction) {
  Module M("Mod", State.getCtx());
  initModule(M, State);

  constexpr StringLiteral Section = ".text.test";
  auto &F = State.createFunction(M, "func", Section, Function::ExternalLinkage);

  EXPECT_EQ(F.getName(), "func");
  EXPECT_EQ(F.getSection(), Section);
  EXPECT_EQ(F.getLinkage(), Function::ExternalLinkage);
  EXPECT_TRUE(F.doesNotThrow());
  EXPECT_EQ(F.arg_size(), 0u);

  // The function is actually registered in the module and retrievable by name,
  // and it carries the expected void() signature created in the state context.
  EXPECT_EQ(M.getFunction("func"), &F);
  ASSERT_TRUE(F.getReturnType()->isVoidTy());
  EXPECT_TRUE(F.empty());
}

TEST_P(LLVMStateTest, CreateFunctionDefaultSection) {
  Module M("Mod", State.getCtx());
  initModule(M, State);

  // An empty section name means "use the default" so no section is attached to
  // the resulting function.
  auto &F = State.createFunction(M, "func", /*SectionName=*/"",
                                 Function::InternalLinkage);

  EXPECT_EQ(F.getName(), "func");
  EXPECT_TRUE(F.getSection().empty());
  EXPECT_EQ(F.getLinkage(), Function::InternalLinkage);
  EXPECT_TRUE(F.doesNotThrow());
}

//===--------------------- createMachineFunctionFor ----------------------===//

TEST_P(LLVMStateTest, CreateMachineFunctionForDefaultSection) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());

  auto &F = State.createFunction(M, "func", "", Function::ExternalLinkage);
  auto &MF = State.createMachineFunctionFor(F, MMI);

  // With an empty section name the default ".text" is used, i.e. no section is
  // attached at the IR level.
  EXPECT_TRUE(F.getSection().empty());

  // Resulting function properties.
  EXPECT_FALSE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::IsSSA));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoVRegs));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoPHIs));

  // The injected empty IR body (a single basic block with a return).
  ASSERT_EQ(&F, &MF.getFunction());
  EXPECT_EQ(F.size(), 1u);
  ASSERT_TRUE(isa<ReturnInst>(&F.getEntryBlock().back()));
}

TEST_P(LLVMStateTest, CreateMachineFunctionForWiring) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());

  auto &F = State.createFunction(M, "func", "", Function::ExternalLinkage);
  // MMI wiring: retrieving the function again through the public API yields the
  // very same MachineFunction object.
  auto &MF1 = State.createMachineFunctionFor(F, MMI);
  auto &MF2 = State.createMachineFunctionFor(F, MMI);
  EXPECT_EQ(&MF1, &MF2);
}

TEST_P(LLVMStateTest, CreateMachineFunctionForSetSection) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());

  auto &F = State.createFunction(M, "func", "", Function::ExternalLinkage);
  // With SetSection the function is placed into ".text.<name>".
  auto &MF = State.createMachineFunctionFor(F, MMI, /*SetSection=*/true);

  EXPECT_EQ(F.getSection(), ".text.func");

  EXPECT_FALSE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::IsSSA));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoVRegs));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoPHIs));
}

//===------------------------- createMachineFunction ---------------------===//

TEST_P(LLVMStateTest, CreateMachineFunction) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());

  auto &MF =
      State.createMachineFunction(M, MMI, "func", /*SectionName=*/"",
                                  Function::ExternalLinkage, State.getCtx());

  const Function &F = MF.getFunction();
  EXPECT_EQ(F.getName(), "func");
  // Empty section name => default ".text", no section attached at IR level.
  EXPECT_TRUE(F.getSection().empty());
  EXPECT_EQ(F.getLinkage(), Function::ExternalLinkage);
  EXPECT_TRUE(F.doesNotThrow());

  EXPECT_FALSE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::IsSSA));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoVRegs));
  EXPECT_TRUE(MF.getProperties().hasProperty(
      MachineFunctionProperties::Property::NoPHIs));
}

//===------------------------ createGlobalConstant -----------------------===//

TEST_P(LLVMStateTest, CreateGlobalConstant) {
  // createGlobalConstant builds the variable type in the LLVMState context, so
  // the module must share that context.
  Module M("Mod", State.getCtx());
  initModule(M, State);

  APInt Init(32, 0xDEADBEEF);
  GlobalVariable *GV =
      State.createGlobalConstant(M, Init, GlobalValue::InternalLinkage, "gv");
  ASSERT_NE(GV, nullptr);

  EXPECT_EQ(GV->getName(), "gv");
  EXPECT_EQ(GV->getLinkage(), GlobalValue::InternalLinkage);
  EXPECT_TRUE(GV->isConstant());
  EXPECT_TRUE(GV->hasInitializer());
  EXPECT_EQ(GV->getValueType()->getIntegerBitWidth(), 32u);
  EXPECT_EQ(GV->getAddressSpace(), 0u);

  auto *InitVal = dyn_cast<ConstantInt>(GV->getInitializer());
  ASSERT_NE(InitVal, nullptr);
  EXPECT_EQ(InitVal->getValue(), Init);

  // The global is inserted into (and retrievable from) the module.
  EXPECT_EQ(M.getNamedGlobal("gv"), GV);
}

TEST_P(LLVMStateTest, CreateGlobalMutable) {
  Module M("Mod", State.getCtx());
  initModule(M, State);

  // With IsConstant == false the variable must be reported as non-constant; in
  // the final ELF this becomes writable data rather than .rodata.
  APInt Init(8, 0xAB);
  auto *GV = State.createGlobalConstant(M, Init, GlobalValue::InternalLinkage,
                                        "mutable", /*IsConstant=*/false);
  ASSERT_NE(GV, nullptr);
  EXPECT_FALSE(GV->isConstant());
  EXPECT_TRUE(GV->hasInitializer());
  EXPECT_EQ(M.getNamedGlobal("mutable"), GV);
}

TEST_P(LLVMStateTest, CreateGlobalConstantDecl) {
  // createGlobalConstantDecl builds the variable type in the LLVMState
  // context, so the module must share that context.
  Module M("Mod", State.getCtx());
  initModule(M, State);

  GlobalVariable *GV = State.createGlobalConstantDecl(
      M, /*BitWidth=*/64, GlobalValue::ExternalLinkage, "gv_decl");
  ASSERT_NE(GV, nullptr);

  EXPECT_EQ(GV->getName(), "gv_decl");
  EXPECT_EQ(GV->getLinkage(), GlobalValue::ExternalLinkage);
  EXPECT_TRUE(GV->isConstant());
  // A declaration has no initialiser.
  EXPECT_FALSE(GV->hasInitializer());
  EXPECT_EQ(GV->getValueType()->getIntegerBitWidth(), 64u);
  EXPECT_EQ(M.getNamedGlobal("gv_decl"), GV);
}

//===-------------------------- code-size estimation ---------------------===//

TEST_P(LLVMStateTest, GetCodeBlockSizeEmptyRange) {
  // The behaviour of getCodeBlockSize over an empty range is that it returns 0
  // without dereferencing anything.
  ArrayRef<MachineInstr> Empty;
  EXPECT_EQ(State.getCodeBlockSize(Empty.begin(), Empty.end()), 0u);
}

TEST_P(LLVMStateTest, EmptyMachineFunctionHasZeroSize) {
  Module M("Mod", State.getCtx());
  initModule(M, State);
  MachineModuleInfo MMI(&State.getTargetMachine());

  // A freshly-created MachineFunction has no machine blocks until codegen
  // lowers the injected IR body, so its aggregated size must be zero.
  auto &MF =
      State.createMachineFunction(M, MMI, "func", /*SectionName=*/"",
                                  Function::ExternalLinkage, State.getCtx());
  EXPECT_TRUE(MF.empty());
  EXPECT_EQ(State.getFunctionSize(MF), 0u);

  // The same holds for the per-block view of a single, still-empty block.
  auto *MBB = MF.CreateMachineBasicBlock();
  MF.push_back(MBB);
  EXPECT_TRUE(MBB->empty());
  EXPECT_EQ(State.getMBBSize(*MBB), 0u);
}

//===-------------------- suite instantiation ----------------------------===//

namespace {

// The target-independent tests are parameterized over every enabled backend.
// Build the parameter list from SnippyTargets.def (generated from
// LLVM_SNIPPY_TARGETS_LIST) so that only backends that were actually built are
// exercised; attempting to create a state for a backend that is not linked
// would fail at runtime.
std::string getTripleForTarget(StringRef TargetName) {
  if (TargetName == "RISCV")
    return "riscv64";
  if (TargetName == "X86")
    return "x86_64";
  if (TargetName == "AArch64")
    return "aarch64";
  llvm_unreachable("unexpected SnippyTarget name");
}

std::vector<SelectedTargetInfo> getTestTargets() {
  std::vector<SelectedTargetInfo> Targets;
#define SNIPPY_TARGET(TN)                                                      \
  Targets.emplace_back(                                                        \
      SelectedTargetInfo { getTripleForTarget(#TN), "", "", "" });
#include "llvm/Config/SnippyTargets.def"
#undef SNIPPY_TARGET
  return Targets;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(SnippyBackend, LLVMStateTest,
                         ::testing::ValuesIn(getTestTargets()));

} // namespace llvm::snippy
