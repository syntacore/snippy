//===-- LLVMState.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A class to set up and access common LLVM objects.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/RandUtil.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/Object/ObjectFile.h"

#include <memory>
#include <string>

namespace llvm {

class AsmPrinter;
class MCCodeEmitter;
class MCDisassembler;

namespace snippy {

class SnippyTarget;

struct SelectedTargetInfo final {
  std::string Triple;
  std::string CPU;
  std::string Features;
};

// An object to initialize LLVM and prepare objects needed to run the
// measurements.
class LLVMState {
public:
  // uses specified triple
  LLVMState(const SelectedTargetInfo &TargetInfo);

  ~LLVMState();

  LLVMTargetMachine &getTargetMachine() const {
    assert(TheTargetMachine);
    return *TheTargetMachine;
  }
  std::unique_ptr<LLVMTargetMachine> createLLVMTargetMachine() const;

  const SnippyTarget &getSnippyTarget() const {
    assert(TheSnippyTarget);
    return *TheSnippyTarget;
  }

  bool canAssemble(const MCInst &mc_inst) const;

  // For convenience:
  const MCInstrInfo &getInstrInfo() const {
    return *TheTargetMachine->getMCInstrInfo();
  }
  const MCRegisterInfo &getRegInfo() const {
    return *TheTargetMachine->getMCRegisterInfo();
  }
  const MCSubtargetInfo &getSubtargetInfo() const {
    return *TheTargetMachine->getMCSubtargetInfo();
  }

  Function &createFunction(Module &M, StringRef FunctionName,
                           StringRef SectionName,
                           Function::LinkageTypes Linkage,
                           LLVMContext &ExternalCtx) const {
    auto *FT = FunctionType::get(Type::getVoidTy(ExternalCtx), false);
    auto *F = Function::Create(FT, Linkage, FunctionName, M);
    // Assign specific output section for this function
    // if not empty. Default output section is ".text".
    if (!SectionName.empty())
      F->setSection(SectionName);
    return *F;
  }

  Function &createFunction(Module &M, StringRef FunctionName,
                           StringRef SectionName,
                           Function::LinkageTypes Linkage) {
    return createFunction(M, FunctionName, SectionName, Linkage, Ctx);
  }

  MachineFunction &createMachineFunctionFor(Function &F, MachineModuleInfo &MMI,
                                            LLVMContext &ExternalCtx) const {
    auto *BB = BasicBlock::Create(ExternalCtx, "", &F);
    ReturnInst::Create(ExternalCtx, BB);
    F.setIsMaterializable(true);
    auto &MF = MMI.getOrCreateMachineFunction(F);
    auto &Props = MF.getProperties();
    Props.set(MachineFunctionProperties::Property::NoVRegs);
    Props.reset(MachineFunctionProperties::Property::IsSSA);
    Props.set(MachineFunctionProperties::Property::NoPHIs);
    return MF;
  }

  MachineFunction &createMachineFunctionFor(Function &F,
                                            MachineModuleInfo &MMI) {
    return createMachineFunctionFor(F, MMI, Ctx);
  }

  MachineFunction &createMachineFunction(Module &M, MachineModuleInfo &MMI,
                                         StringRef FunctionName,
                                         StringRef SectionName,
                                         Function::LinkageTypes Linkage,
                                         LLVMContext &ExternalCtx) const {
    auto &F =
        createFunction(M, FunctionName, SectionName, Linkage, ExternalCtx);
    return createMachineFunctionFor(F, MMI, ExternalCtx);
  }

  //  "Assigning a value into the constant leads to undefined behavior"-llvm doc
  //  IsConstant flag affects section flags in the final ELF
  GlobalVariable *createGlobalConstant(
      Module &M, APInt const &Init,
      GlobalValue::LinkageTypes Linkage = GlobalValue::InternalLinkage,
      StringRef Name = "global", bool IsConstant = true) {
    auto *VarType = Type::getIntNTy(Ctx, Init.getBitWidth());
    Constant *VarInit = ConstantInt::get(Ctx, Init);
    auto *GV =
        new GlobalVariable(M, VarType, IsConstant, Linkage, VarInit, Name);
    return GV;
  }

  AsmPrinter &getOrCreateAsmPrinter() const;
  MCCodeEmitter &getCodeEmitter() const;
  MCDisassembler &getDisassembler() const;
  LLVMContext &getCtx() { return Ctx; }

private:
  const SnippyTarget *TheSnippyTarget;
  std::unique_ptr<LLVMTargetMachine> TheTargetMachine;
  std::unique_ptr<MCContext> TheContext;
  mutable std::unique_ptr<AsmPrinter> TheAsmPrinter;
  std::unique_ptr<MCCodeEmitter> TheCodeEmitter;
  std::unique_ptr<MCDisassembler> TheDisassembler;
  LLVMContext Ctx;
};

} // namespace snippy
} // namespace llvm
