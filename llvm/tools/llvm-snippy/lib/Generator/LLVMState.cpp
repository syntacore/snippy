//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/LLVMState.h"

#include "snippy/Target/Target.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

namespace llvm {
namespace snippy {

LLVMState::LLVMState(const SelectedTargetInfo &TargetInfo) {
  std::string Error;
  const Target *const TheTarget =
      TargetRegistry::lookupTarget(TargetInfo.Triple, Error);
  if (!TheTarget)
    report_fatal_error(Twine(Error));
  const TargetOptions Options;
  TheTargetMachine.reset(static_cast<LLVMTargetMachine *>(
      TheTarget->createTargetMachine(TargetInfo.Triple, TargetInfo.CPU,
                                     TargetInfo.Features, Options,
                                     Reloc::Model::Static)));
  assert(TheTargetMachine && "unable to create target machine");
  auto TT = TheTargetMachine->getTargetTriple();
  TheSnippyTarget = SnippyTarget::lookup(TT);
  if (!TheSnippyTarget) {
    errs() << "no snippy target for " << TargetInfo.Triple << "\n";
    report_fatal_error("sorry, target is not implemented", false);
  }
  const Target &T = TheTargetMachine->getTarget();
  const auto *STI = TheTargetMachine->getMCSubtargetInfo();
  TheContext =
      std::make_unique<MCContext>(TT, TheTargetMachine->getMCAsmInfo(),
                                  TheTargetMachine->getMCRegisterInfo(), STI);
  TheCodeEmitter = std::unique_ptr<MCCodeEmitter>(
      T.createMCCodeEmitter(*TheTargetMachine->getMCInstrInfo(), *TheContext));
  TheDisassembler = std::unique_ptr<MCDisassembler>(
      T.createMCDisassembler(*STI, *TheContext));
}

LLVMState::~LLVMState() {}

std::unique_ptr<LLVMTargetMachine> LLVMState::createLLVMTargetMachine() const {
  return std::unique_ptr<LLVMTargetMachine>(static_cast<LLVMTargetMachine *>(
      TheTargetMachine->getTarget().createTargetMachine(
          TheTargetMachine->getTargetTriple().normalize(),
          TheTargetMachine->getTargetCPU(),
          TheTargetMachine->getTargetFeatureString(), TheTargetMachine->Options,
          Reloc::Model::Static)));
}

AsmPrinter &LLVMState::getOrCreateAsmPrinter() const {
  if (TheAsmPrinter)
    return *TheAsmPrinter;
  const auto &T = TheTargetMachine->getTarget();

  auto *NullStreamer = T.createNullStreamer(*TheContext);
  std::unique_ptr<MCStreamer> Streamer{NullStreamer};
  TheAsmPrinter = std::unique_ptr<AsmPrinter>(
      T.createAsmPrinter(*TheTargetMachine, std::move(Streamer)));
  return *TheAsmPrinter;
}

MCCodeEmitter &LLVMState::getCodeEmitter() const {
  assert(TheCodeEmitter);
  return *TheCodeEmitter;
}

MCDisassembler &LLVMState::getDisassembler() const {
  assert(TheDisassembler && "Unexpected nullptr");
  return *TheDisassembler;
}

bool LLVMState::canAssemble(const MCInst &Inst) const {
  std::unique_ptr<const MCCodeEmitter> CodeEmitter(
      TheTargetMachine->getTarget().createMCCodeEmitter(
          *TheTargetMachine->getMCInstrInfo(), *TheContext));
  assert(CodeEmitter && "unable to create code emitter");
  SmallVector<char, 16> Tmp;
  SmallVector<MCFixup, 4> Fixups;
  CodeEmitter->encodeInstruction(Inst, Tmp, Fixups,
                                 *TheTargetMachine->getMCSubtargetInfo());
  return Tmp.size() > 0;
}

} // namespace snippy
} // namespace llvm
