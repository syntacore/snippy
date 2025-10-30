//===-- LLVMState.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/GeneratorUtils/LLVMState.h"

#include "snippy/Support/Error.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/RISCVISAInfo.h"

namespace llvm {
namespace snippy {

LLVMState::LLVMState(const SnippyTarget *SnippyTarget,
                     std::unique_ptr<TargetMachine> TargetMachine,
                     std::unique_ptr<MCContext> Context,
                     std::unique_ptr<MCCodeEmitter> CodeEmitter,
                     std::unique_ptr<MCDisassembler> Disassembler)
    : Ctx(std::make_unique<LLVMContext>()), TheSnippyTarget(SnippyTarget),
      TheTargetMachine(std::move(TargetMachine)),
      TheContext(std::move(Context)), TheCodeEmitter(std::move(CodeEmitter)),
      TheDisassembler(std::move(Disassembler)) {}

static Expected<std::string> getRISCVFeaturesFromMArch(StringRef MArch) {
  auto ISAInfo = RISCVISAInfo::parseArchString(
      MArch, /*EnableExperimentalExtension=*/true);
  if (!ISAInfo)
    return ISAInfo.takeError();
  assert(ISAInfo->get());
  auto ISAFeatures = ISAInfo->get()->toFeatures();
  std::string Buffer;
  raw_string_ostream FeatOS(Buffer);
  ListSeparator LS(",");
  for (const auto &Feat : ISAFeatures)
    FeatOS << LS << Feat;

  return FeatOS.str();
}

static Expected<std::string>
getTargetFeaturesFromMArch(Triple::ArchType ArchType, StringRef MArch) {
  assert(ArchType != Triple::ArchType::UnknownArch);
  switch (ArchType) {
  case Triple::ArchType::riscv32:
  case Triple::ArchType::riscv64:
    return getRISCVFeaturesFromMArch(MArch);
  default:
    return makeFailure(Errc::Unimplemented,
                       "march is not implemented for this target");
  }
}

static bool checkTriple(const Triple &TheTriple) {
  return !TheTriple.getTriple().empty() &&
         TheTriple.getArch() != Triple::ArchType::UnknownArch;
}

struct DeducedTriple {
  Triple TheTriple;
  bool MArchIsTriple;
};

static auto deduceTriple(StringRef MTriple, StringRef MArch) {
  bool MArchIsTriple = false;
  Triple TheTriple(MTriple);
  if (!MTriple.empty() || MArch.empty())
    return DeducedTriple{TheTriple, MArchIsTriple};

  assert(!checkTriple(TheTriple));
  Triple TripleFromMArch(MArch);
  if (!checkTriple(TripleFromMArch))
    return DeducedTriple{TheTriple, MArchIsTriple};

  MArchIsTriple = true;
  return DeducedTriple{TripleFromMArch, MArchIsTriple};
}

Expected<LLVMState> LLVMState::create(const SelectedTargetInfo &TargetInfo) {
  auto [TheTriple, MArchIsTriple] =
      deduceTriple(TargetInfo.Triple, TargetInfo.MArch);
  if (!checkTriple(TheTriple))
    return makeFailure(Errc::InvalidArgument,
                       TheTriple.getTriple().empty()
                           ? "target triple is not specified"
                           : "unknown target specified");

  if (MArchIsTriple)
    snippy::warn(WarningName::MArchIsTriple,
                 "'march' with triple value is deprecated",
                 "use 'mtriple' option instead");

  auto TripleStr = TheTriple.normalize();
  std::string Error;
  const Target *Tgt = TargetRegistry::lookupTarget(TripleStr, Error);

  if (!Tgt)
    return makeFailure(Errc::InvalidConfiguration, Twine(Error));

  std::string TargetFeatures;
  if (!MArchIsTriple && !TargetInfo.MArch.empty()) {
    auto ExpectedFeatures =
        getTargetFeaturesFromMArch(TheTriple.getArch(), TargetInfo.MArch);
    if (!ExpectedFeatures)
      return makeFailure(Errc::InvalidArgument,
                         formatv("Invalid march: {0}",
                                 toString(ExpectedFeatures.takeError())));
    TargetFeatures = std::move(ExpectedFeatures.get());
  }

  TargetFeatures += ",";
  TargetFeatures += TargetInfo.Features;
  // Relax feature is enabled by default to enable desired
  // type of relocations be produced by linker that AsmPrinter
  // cannot do by itself on some targets.
  // E.G.: RISCV AsmPrinter cannot emit JAL directly.
  TargetFeatures += ",+relax";
  const TargetOptions Options;
  auto TM = std::unique_ptr<TargetMachine>(static_cast<TargetMachine *>(
      Tgt->createTargetMachine(TripleStr, TargetInfo.CPU, TargetFeatures,
                               Options, Reloc::Model::Static)));
  if (!TM)
    return makeFailure(Errc::Failure, "Unable to create target machine");

  auto TT = TM->getTargetTriple();
  const auto *SnippyTgt = SnippyTarget::lookup(TT);
  if (!SnippyTgt)
    return makeFailure(Errc::InvalidConfiguration,
                       llvm::formatv("No snippy target for {0}", TripleStr));

  const Target &T = TM->getTarget();
  const auto *STI = TM->getMCSubtargetInfo();

  if (auto &CPU = TargetInfo.CPU; !CPU.empty() && !STI->isCPUStringValid(CPU))
    return makeFailure(
        Errc::InvalidConfiguration,
        llvm::formatv("cpu '{0}' is not valid for the specified subtarget",
                      TargetInfo.CPU));

  auto MCCtx = std::make_unique<MCContext>(TheTriple, TM->getMCAsmInfo(),
                                           TM->getMCRegisterInfo(), STI);
  auto *MCII = TM->getMCInstrInfo();
  assert(MCII);
  auto CodeEmitter =
      std::unique_ptr<MCCodeEmitter>(T.createMCCodeEmitter(*MCII, *MCCtx));
  auto Disassembler =
      std::unique_ptr<MCDisassembler>(T.createMCDisassembler(*STI, *MCCtx));

  return LLVMState(SnippyTgt, std::move(TM), std::move(MCCtx),
                   std::move(CodeEmitter), std::move(Disassembler));
}

LLVMState::~LLVMState() {}

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

MCInstPrinter &LLVMState::getInstPrinter() const {
  if (TheInstPrinter)
    return *TheInstPrinter;
  assert(TheTargetMachine);
  auto &TM = *TheTargetMachine;
  const Target &T = TM.getTarget();
  auto &AsmInfo = *TM.getMCAsmInfo();

  TheInstPrinter.reset(
      T.createMCInstPrinter(TM.getTargetTriple(), AsmInfo.getAssemblerDialect(),
                            AsmInfo, getInstrInfo(), getRegInfo()));
  return *TheInstPrinter;
}

std::unique_ptr<MCStreamer> LLVMState::createObjStreamer(raw_pwrite_stream &OS,
                                                         MCContext &MCCtx) {
  assert(TheTargetMachine);
  auto MCStreamerOrErr = TheTargetMachine->createMCStreamer(
      OS, nullptr, CodeGenFileType::ObjectFile, MCCtx);
  if (!MCStreamerOrErr)
    // FIXME: Make failable and return Expected<std::unique_ptr<MCStreamer>>
    // instead.
    snippy::fatal(getCtx(), "Internal Error creating MCStreamer",
                  toString(MCStreamerOrErr.takeError()));

  // We explicitly specified ObjectFile type 3 lines above so we can down-cast
  // it safely
  auto *ObjStreamer =
      static_cast<MCObjectStreamer *>(MCStreamerOrErr->release());
  ObjStreamer->setEmitEHFrame(false);
  ObjStreamer->setEmitDebugFrame(false);
  return std::unique_ptr<MCStreamer>(ObjStreamer);
}

} // namespace snippy
} // namespace llvm
