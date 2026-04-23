//===- AArch64AsmPrinter.h - Interface of AArch64 Asm Printer--------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64ASMPRINTER_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64ASMPRINTER_H

#include "AArch64MCInstLower.h"
#include "AArch64MachineFunctionInfo.h"
#include "MCTargetDesc/AArch64TargetStreamer.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/FaultMaps.h"
#include "llvm/MC/MCStreamer.h"

using namespace llvm;

class AArch64AsmPrinter : public AsmPrinter {
  AArch64MCInstLower MCInstLowering;
  FaultMaps FM;
  const AArch64Subtarget *STI;
  bool ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags = false;
#ifndef NDEBUG
  unsigned InstsEmitted;
#endif
  bool EnableImportCallOptimization = false;
  DenseMap<MCSection *, std::vector<std::pair<MCSymbol *, MCSymbol *>>>
      SectionToImportedFunctionCalls;

public:
  static char ID;

  AArch64AsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID),
        MCInstLowering(OutContext, *this), FM(*this) {}

  StringRef getPassName() const override { return "AArch64 Assembly Printer"; }

  /// Wrapper for MCInstLowering.lowerOperand() for the
  /// tblgen'erated pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const {
    return MCInstLowering.lowerOperand(MO, MCOp);
  }

  const MCExpr *lowerConstantPtrAuth(const ConstantPtrAuth &CPA) override;

  const MCExpr *lowerBlockAddressConstant(const BlockAddress &BA) override;

  void emitStartOfAsmFile(Module &M) override;
  void emitJumpTableImpl(const MachineJumpTableInfo &MJTI,
                         ArrayRef<unsigned> JumpTableIndices) override;
  std::tuple<const MCSymbol *, uint64_t, const MCSymbol *,
             codeview::JumpTableEntrySize>
  getCodeViewJumpTableInfo(int JTI, const MachineInstr *BranchInstr,
                           const MCSymbol *BranchLabel) const override;

  void emitFunctionEntryLabel() override;

  void emitXXStructor(const DataLayout &DL, const Constant *CV) override;

  void LowerJumpTableDest(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerHardenedBRJumpTable(const MachineInstr &MI);

  void LowerMOPS(MCStreamer &OutStreamer, const MachineInstr &MI);

  void LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                     const MachineInstr &MI);
  void LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  void LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);
  void LowerFAULTING_OP(const MachineInstr &MI);

  void LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr &MI);
  void LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr &MI);
  void LowerPATCHABLE_TAIL_CALL(const MachineInstr &MI);
  void LowerPATCHABLE_EVENT_CALL(const MachineInstr &MI, bool Typed);

  typedef std::tuple<unsigned, bool, uint32_t, bool, uint64_t>
      HwasanMemaccessTuple;
  std::map<HwasanMemaccessTuple, MCSymbol *> HwasanMemaccessSymbols;
  void LowerKCFI_CHECK(const MachineInstr &MI);
  void LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI);
  void emitHwasanMemaccessSymbols(Module &M);

  void emitSled(const MachineInstr &MI, SledKind Kind);

  // Emit the sequence for BRA/BLRA (authenticate + branch/call).
  void emitPtrauthBranch(const MachineInstr *MI);

  void emitPtrauthCheckAuthenticatedValue(Register TestedReg,
                                          Register ScratchReg,
                                          AArch64PACKey::ID Key,
                                          AArch64PAuth::AuthCheckMethod Method,
                                          bool ShouldTrap,
                                          const MCSymbol *OnFailure);

  // Check authenticated LR before tail calling.
  void emitPtrauthTailCallHardening(const MachineInstr *TC);

  // Emit the sequence for AUT or AUTPAC.
  void emitPtrauthAuthResign(Register AUTVal, AArch64PACKey::ID AUTKey,
                             uint64_t AUTDisc,
                             const MachineOperand *AUTAddrDisc,
                             Register Scratch,
                             std::optional<AArch64PACKey::ID> PACKey,
                             uint64_t PACDisc, Register PACAddrDisc);

  // Emit the sequence to compute the discriminator.
  //
  // The returned register is either unmodified AddrDisc or ScratchReg.
  //
  // If the expanded pseudo is allowed to clobber AddrDisc register, setting
  // MayUseAddrAsScratch may save one MOV instruction, provided the address
  // is already in x16/x17 (i.e. return x16/x17 which is the *modified* AddrDisc
  // register at the same time) or the OS doesn't make it safer to use x16/x17
  // (see AArch64Subtarget::isX16X17Safer()):
  //
  //   mov   x17, x16
  //   movk  x17, #1234, lsl #48
  //   ; x16 is not used anymore
  //
  // can be replaced by
  //
  //   movk  x16, #1234, lsl #48
  Register emitPtrauthDiscriminator(uint16_t Disc, Register AddrDisc,
                                    Register ScratchReg,
                                    bool MayUseAddrAsScratch = false);

  // Emit the sequence for LOADauthptrstatic
  void LowerLOADauthptrstatic(const MachineInstr &MI);

  // Emit the sequence for LOADgotPAC/MOVaddrPAC (either GOT adrp-ldr or
  // adrp-add followed by PAC sign)
  void LowerMOVaddrPAC(const MachineInstr &MI);

  // Emit the sequence for LOADgotAUTH (load signed pointer from signed ELF GOT
  // and authenticate it with, if FPAC bit is not set, check+trap sequence after
  // authenticating)
  void LowerLOADgotAUTH(const MachineInstr &MI);

  /// tblgen'erated driver function for lowering simple MI->MC
  /// pseudo instructions.
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  // Emit Build Attributes
  void emitAttributes(unsigned Flags, uint64_t PAuthABIPlatform,
                      uint64_t PAuthABIVersion, AArch64TargetStreamer *TS);

  // Emit expansion of Compare-and-branch pseudo instructions
  void emitCBPseudoExpansion(const MachineInstr *MI);

  void EmitToStreamer(MCStreamer &S, const MCInst &Inst);
  void EmitToStreamer(const MCInst &Inst) {
    EmitToStreamer(*OutStreamer, Inst);
  }

  void emitInstruction(const MachineInstr *MI) override;

  void emitFunctionHeaderComment() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AsmPrinter::getAnalysisUsage(AU);
    AU.setPreservesAll();
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (auto *PSIW = getAnalysisIfAvailable<ProfileSummaryInfoWrapperPass>())
      PSI = &PSIW->getPSI();
    if (auto *SDPIW =
            getAnalysisIfAvailable<StaticDataProfileInfoWrapperPass>())
      SDPI = &SDPIW->getStaticDataProfileInfo();

    AArch64FI = MF.getInfo<AArch64FunctionInfo>();
    STI = &MF.getSubtarget<AArch64Subtarget>();

    SetupMachineFunction(MF);

    if (STI->isTargetCOFF()) {
      bool Local = MF.getFunction().hasLocalLinkage();
      COFF::SymbolStorageClass Scl =
          Local ? COFF::IMAGE_SYM_CLASS_STATIC : COFF::IMAGE_SYM_CLASS_EXTERNAL;
      int Type = COFF::IMAGE_SYM_DTYPE_FUNCTION << COFF::SCT_COMPLEX_TYPE_SHIFT;

      OutStreamer->beginCOFFSymbolDef(CurrentFnSym);
      OutStreamer->emitCOFFSymbolStorageClass(Scl);
      OutStreamer->emitCOFFSymbolType(Type);
      OutStreamer->endCOFFSymbolDef();
    }

    // Emit the rest of the function body.
    emitFunctionBody();

    // Emit the XRay table for this function.
    emitXRayTable();

    // We didn't modify anything.
    return false;
  }

  const MCExpr *lowerConstant(const Constant *CV,
                              const Constant *BaseCV = nullptr,
                              uint64_t Offset = 0) override;

private:
  void printOperand(const MachineInstr *MI, unsigned OpNum, raw_ostream &O);
  bool printAsmMRegister(const MachineOperand &MO, char Mode, raw_ostream &O);
  bool printAsmRegInClass(const MachineOperand &MO,
                          const TargetRegisterClass *RC, unsigned AltName,
                          raw_ostream &O);

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                       const char *ExtraCode, raw_ostream &O) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNum,
                             const char *ExtraCode, raw_ostream &O) override;

  void PrintDebugValueComment(const MachineInstr *MI, raw_ostream &OS);

  void emitFunctionBodyEnd() override;
  void emitGlobalAlias(const Module &M, const GlobalAlias &GA) override;

  MCSymbol *GetCPISymbol(unsigned CPID) const override;
  void emitEndOfAsmFile(Module &M) override;

  AArch64FunctionInfo *AArch64FI = nullptr;

  /// Emit the LOHs contained in AArch64FI.
  void emitLOHs();

  void emitMovXReg(Register Dest, Register Src);
  void emitMOVZ(Register Dest, uint64_t Imm, unsigned Shift);
  void emitMOVK(Register Dest, uint64_t Imm, unsigned Shift);

  /// Emit instruction to set float register to zero.
  void emitFMov0(const MachineInstr &MI);

  using MInstToMCSymbol = std::map<const MachineInstr *, MCSymbol *>;

  MInstToMCSymbol LOHInstToLabel;

  bool shouldEmitWeakSwiftAsyncExtendedFramePointerFlags() const override {
    return ShouldEmitWeakSwiftAsyncExtendedFramePointerFlags;
  }

  const MCSubtargetInfo *getIFuncMCSubtargetInfo() const override {
    assert(STI);
    return STI;
  }
  void emitMachOIFuncStubBody(Module &M, const GlobalIFunc &GI,
                              MCSymbol *LazyPointer) override;
  void emitMachOIFuncStubHelperBody(Module &M, const GlobalIFunc &GI,
                                    MCSymbol *LazyPointer) override;

  /// Checks if this instruction is part of a sequence that is eligle for import
  /// call optimization and, if so, records it to be emitted in the import call
  /// section.
  void recordIfImportCall(const MachineInstr *BranchInst);
};

#endif
