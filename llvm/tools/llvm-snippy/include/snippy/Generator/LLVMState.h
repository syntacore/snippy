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
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
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
    const auto *Ret = TheTargetMachine->getMCSubtargetInfo();
    assert(Ret);
    return *Ret;
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
    F->setDoesNotThrow();
    return *F;
  }

  Function &createFunction(Module &M, StringRef FunctionName,
                           StringRef SectionName,
                           Function::LinkageTypes Linkage) {
    return createFunction(M, FunctionName, SectionName, Linkage, Ctx);
  }

  MachineFunction &createMachineFunctionFor(Function &F, MachineModuleInfo &MMI,
                                            LLVMContext &ExternalCtx,
                                            bool SetSection = false) const {
    auto *BB = BasicBlock::Create(ExternalCtx, "", &F);
    ReturnInst::Create(ExternalCtx, BB);
    F.setIsMaterializable(true);
    if (SetSection)
      F.setSection(Twine(".text.").concat(F.getName()).str());
    auto &MF = MMI.getOrCreateMachineFunction(F);
    auto &Props = MF.getProperties();
    Props.set(MachineFunctionProperties::Property::NoVRegs);
    Props.reset(MachineFunctionProperties::Property::IsSSA);
    Props.set(MachineFunctionProperties::Property::NoPHIs);
    return MF;
  }

  MachineFunction &createMachineFunctionFor(Function &F, MachineModuleInfo &MMI,
                                            bool SetSection = false) {
    return createMachineFunctionFor(F, MMI, Ctx, SetSection);
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

  std::unique_ptr<MCStreamer> createObjStreamer(raw_pwrite_stream &OS,
                                                MCContext &Ctx);

  AsmPrinter &getOrCreateAsmPrinter() const;
  MCCodeEmitter &getCodeEmitter() const;
  MCDisassembler &getDisassembler() const;
  LLVMContext &getCtx() { return Ctx; }

  const TargetSubtargetInfo &getSubtargetImpl(const Function &Fn) const {
    return *getTargetMachine().getSubtargetImpl(Fn);
  }

  template <typename SubtargetType>
  const SubtargetType &getSubtarget(const Function &Fn) const {
    return static_cast<const SubtargetType &>(getSubtargetImpl(Fn));
  }

  template <typename SubtargetType>
  const SubtargetType &getSubtarget(const MachineFunction &Fn) const {
    return static_cast<const SubtargetType &>(
        getSubtargetImpl(Fn.getFunction()));
  }

  template <typename It> size_t getCodeBlockSize(It Begin, It End) {
    auto SizeAccumulator = [this](auto CurrSize, auto &MI) {
      size_t InstrSize = getSnippyTarget().getInstrSize(MI, *this);
      if (InstrSize == 0)
        snippy::warn(
            WarningName::InstructionSizeUnknown, getCtx(),
            [&MI]() {
              std::string Ret;
              llvm::raw_string_ostream OS{Ret};
              OS << "Instruction '";
              MI.print(OS, /* IsStandalone */ true, /* SkipOpers */ true,
                       /* SkipDebugLoc */ true, /* AddNewLine */ false);
              OS << "' has unknown size";
              return Ret;
            }(),
            "function size estimation may be wrong");
      return CurrSize + InstrSize;
    };
    return std::accumulate(Begin, End, 0u, SizeAccumulator);
  }

  size_t getMBBSize(const MachineBasicBlock &MBB) {
    return getCodeBlockSize(MBB.begin(), MBB.end());
  }

  size_t getFunctionSize(const MachineFunction &MF) {
    return std::accumulate(MF.begin(), MF.end(), 0ul,
                           [this](auto CurrentSize, const auto &MBB) {
                             return CurrentSize + getMBBSize(MBB);
                           });
  }

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
