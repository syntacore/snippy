//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Target/Target.h"

#include "AArch64.h"
#include "AArch64AsmGenerator.h"
#include "AArch64AsmPrinter.h"
#include "AArch64ExpandImm.h"
#include "AArch64Generated.h"
#include "AArch64InstrInfo.h"
#include "AArch64RegisterInfo.h"
#include "AArch64Subtarget.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "TargetConfig.h"
#include "TargetGenContext.h"
#include "Utils/AArch64BaseInfo.h"
#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Simulator/RISCVRegTypes.h"
#include "snippy/Simulator/Targets/AArch64.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCRegister.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/FormatVariadic.h"

#include <vector>

// FIX: experimental AArch64 support, remove after bringup
#define SNIPPY_UNIMPLEMENTED()                                                 \
  snippy::fatal(llvm::Twine("AArch64 experimental: ") + __PRETTY_FUNCTION__ +  \
                " at " + __FILE__ + ":" + llvm::Twine(__LINE__))

namespace llvm {
namespace snippy {
namespace {

class SnippyAArch64Target : public SnippyTarget {
public:
  SnippyAArch64Target() = default;

  void generateWriteValueSeq(InstructionGenerationContext &IGC, APInt Value,
                             MCRegister DestReg,
                             SmallVectorImpl<MCInst> &Insts) const override {
    assert(Value.getBitWidth() <= 64);
    // TODO: check is it possible to use pseudo here (potential issues with
    // selfcheck)
    auto Inst =
        (Value.getBitWidth() <= 32) ? AArch64::MOVi32imm : AArch64::MOVi64imm;
    auto InstBuilder = MCInstBuilder(Inst).addReg(DestReg);
    InstBuilder.addImm(Value.getZExtValue());
    Insts.push_back(InstBuilder);
  }

  [[noreturn]] void reportUnimplementedError() const {
    snippy::fatal("Experimental AArch64 support");
  }

  bool matchesArch(Triple::ArchType Arch) const override;

  std::unique_ptr<IRegisterState>
  createRegisterState(const TargetGenContextInterface &TgtGenCtx,
                      const TargetSubtargetInfo &ST) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::unique_ptr<TargetGenContextInterface>
  createTargetContext(LLVMState &State, const Config &Cfg,
                      const TargetSubtargetInfo *STI,
                      const RegPoolWrapper &RP) const override {
    return std::make_unique<AArch64GeneratorContext>();
  }

  std::unique_ptr<TargetConfigInterface> createTargetConfig() const override {
    return std::make_unique<AArch64ConfigInterface>();
  }


  void
  checkInstrTargetDependency(const OpcodeHistogram &H, const OpcodeCache &OpCC,
                             const ProgramConfig &ProgramCfg) const override {
    // TODO: add load instr if targets are dependent
  }

  bool isModeSwitchInstr(unsigned Opcode) const override { return false; }

  bool modeSwitchIsSupport(const SnippyProgramContext &ProgCtx) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool needToGenerateModeSwitches(
      const SnippyProgramContext &ProgCtx) const override {
    return false;
  }

  double
  getModeSwitchProbability(const SnippyProgramContext &ProgCtx) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void checkTrackingRestrictions(const OpcodeHistogram &H) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  Error checkOperandsReinitializationSupported(
      unsigned Opcode, const MCInstrInfo &InstrInfo) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::function<bool(unsigned)>
  generateModeChangeAndGetFilter(InstructionGenerationContext &IGC,
                                 MDNode *MetadataMark) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::vector<Register>
  getRegsForSelfcheck(const MachineInstr &MI,
                      InstructionGenerationContext &IGC) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::unique_ptr<SelfcheckTargetConfigInterface>
  createSelfcheckTargetConfig() const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::string
  validateSelfcheckConfig(const SelfcheckConfig &SelfcheckCfg,
                          const OpcodeHistogram &Histogram) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::string getDefaultLastInstr() const override {
    return std::string("RET");
  }

  void generateRegsInit(InstructionGenerationContext &IGC,
                        const IRegisterState &R) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getFPRegsCount(const TargetSubtargetInfo &ST) const override {
    // TODO: Add FPR32
    return AArch64::FPR64RegClass.getNumRegs();
  }

  bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const override {
    LLVMContext Ctx;
    return true;
  }

  void generateCustomInst(
      const MCInstrDesc &InstrDesc,
      planning::InstructionGenerationContext &InstrGenCtx,
      ArrayRef<planning::PreselectedOpInfo> Preselected) const override {

    auto &MBB = InstrGenCtx.MBB;
    auto &ProgCtx = InstrGenCtx.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    const auto &SnippyTgt = State.getSnippyTarget();

    auto MIB =
        getInstBuilder(nullptr, SnippyTgt, MBB, InstrGenCtx.Ins,
                       MBB.getParent()->getFunction().getContext(), InstrDesc);
    // NOTE: moved from randomInstruction generator
    auto NumDefs = InstrDesc.getNumDefs();
    auto TotalNum = InstrDesc.getNumOperands();
    std::vector<planning::PreselectedOpInfo> PreselectedOperands;
    PreselectedOperands.resize(TotalNum);
    assert(PreselectedOperands.size() == TotalNum);
    PreselectedOperands.insert(PreselectedOperands.end(), Preselected.begin(),
                               Preselected.end());
    for (auto &&[OpIdx, OpInfo] : enumerate(PreselectedOperands)) {
      OpInfo.setFlags(OpInfo.getFlags() |
                      (OpIdx < NumDefs ? RegState::Define : 0));
      auto TiedTo = InstrDesc.getOperandConstraint(OpIdx, MCOI::TIED_TO);
      if (TiedTo >= 0)
        OpInfo.setTiedTo(TiedTo);
    }
    AArch64OpndGenerator Gen(InstrGenCtx, InstrDesc);
    Gen.generateOperands(PreselectedOperands);
    auto Operands = Gen.getGeneratedOperands();

    for (auto &Op : Operands)
      MIB.add(Op);
  }
  void instructionPostProcess(InstructionGenerationContext &IGC,
                              MachineInstr &MI) const override {
    // TODO: no need to postprocess for now
  }

  void
  generateCallToMemInitRoutine(InstructionGenerationContext &IGC,
                               size_t SectionStart, size_t SectionSize,
                               MemorySeedTy Seed,
                               const Function &ExternalGFunc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MemInitCallGenResult getSectionStateAfterMemInitRoutine(
      SnippyProgramContext &ProgCtx, const TargetSubtargetInfo &STI,
      size_t SectionSize, MemorySeedTy Seed) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  generateRandomGenFunction(InstructionGenerationContext &IGC) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getRandomGenFunctionMaxSize() const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::vector<unsigned>
  getRegListForMemCpyForSMC(InstructionGenerationContext &IGC) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void generateMemCpyForSMC(MachineFunction &MF,
                            SnippyProgramContext &ProgCtx) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void generateMemorytInitializationAtAddresses(
      InstructionGenerationContext &IGC,
      const MemoryMap &Addresses) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  virtual MachineInstr *generateFinalInst(InstructionGenerationContext &IGC,
                                          unsigned LastInstr) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  const MCRegisterClass &
  getRegClass(const InstructionGenerationContext &IGC,
              unsigned OperandRegClassID, unsigned OpIndex, unsigned Opcode,
              const MCRegisterInfo &RegInfo) const override {
    return RegInfo.getRegClass(OperandRegClassID);
  }

  std::vector<MCRegister> getRegsSuitableForSP() const override {
    // TODO: re-check register class
    std::vector<MCRegister> Result;
    copy(AArch64::GPR64noipRegClass, std::back_inserter(Result));
    return Result;
  }

  std::vector<MCRegister>
  getRegsSuitableForRA(std::optional<unsigned> CallOpcode) const override {
    std::vector<MCRegister> Result;
    copy(AArch64::GPR64commonRegClass, std::back_inserter(Result));
    return Result;
  }

  void getImplicitDefRegs(unsigned Opcode,
                          SmallVectorImpl<MCRegister> &OutRegs) const override {
  }

  MCRegister getStackPointer() const override {
    // TODO: do we need to support 32-bit SP(WSP)
    return AArch64::SP;
  }

  MCRegister getReturnAddress() const override { return AArch64::LR; }

  bool isRegClassSupported(MCRegister Reg) const override {
    return AArch64::GPR64allRegClass.contains(Reg) ||
           AArch64::FPR8RegClass.contains(Reg) ||
           AArch64::FPR16RegClass.contains(Reg) ||
           AArch64::FPR32RegClass.contains(Reg) ||
           AArch64::FPR64RegClass.contains(Reg) ||
           AArch64::FPR128RegClass.contains(Reg);
  }

  void initRegWithSPAddr(InstructionGenerationContext &IGC,
                         MCRegister Reg) const {
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto RP = IGC.pushRegPool();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(AArch64::GPR64RegClassID);
    auto &MBB = IGC.MBB;
    RP->addReserved(getFirstPhysReg(Reg, RI), MBB);
    auto ScratchReg =
        getNonZeroReg("scratch register for stack addr", RI, RegClass, *RP, MBB,
                      AccessMaskBit::SupportRW);
    auto XRegBitSize = getRegBitWidth(ScratchReg, IGC);
    auto &StaticStackCtx = ProgCtx.getStaticStack();
    writeValueToReg(IGC, APInt(XRegBitSize, StaticStackCtx.getSPAddrLocal()),
                    ScratchReg);
    StaticStackCtx.setRegWithSPAddrLocal(ScratchReg);
  }

  void generateSpillToStack(
      InstructionGenerationContext &IGC, MCRegister Reg, MCRegister SP,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &MBB = IGC.MBB;
    assert(ProgCtx.stackEnabled() &&
           "An attempt to generate spill but stack was not enabled.");
    auto SpillSize = static_cast<int64_t>(
        getSpillSizeInBytes(Reg, ProgCtx, IGC.getSubtargetImpl()));

    if (ProgCtx.getConfig().StaticStack) {
      auto &StaticStackCtx = ProgCtx.getStaticStack();
      if (!StaticStackCtx.isSPInReg())
        initRegWithSPAddr(IGC, Reg);
      StaticStackCtx.setSPAddrLocal(StaticStackCtx.getSPAddrLocal() -
                                    SpillSize);
      SP = StaticStackCtx.getRegWithSPAddrLocal();
    }
    auto &Ins = IGC.Ins;
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();
    // Directly produce instruction:
    // sub sp, sp, SpillSize, #lsl 0
    getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(AArch64::SUBXri))
        .addDef(SP)
        .addReg(SP)
        .addImm(SpillSize)
        .addImm(0);

    storeRegToAddrInReg(IGC, SP, Reg);
  }

  unsigned getNonZeroReg(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
                         AccessMaskBit Mask = AccessMaskBit::RW) const {
    // TODO: check how to remove all regs connected with X0
    return RP.getAvailableRegister(
        Desc, RI, RegClass, MBB,
        /*Filter*/
        [](unsigned Reg) { return Reg == AArch64::XZR || Reg == AArch64::WZR; },
        Mask);
  }

  void storeRegToAddrInReg(InstructionGenerationContext &IGC,
                           MCRegister AddrReg, MCRegister Reg,
                           unsigned BytesToWrite = 0) const {
    assert(AArch64::GPR64RegClass.contains(AddrReg) ||
           AArch64::GPR64spRegClass.contains(AddrReg) &&
               "Expected address register be GPR64 or GPR64sp");

    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    const auto &InstrInfo = State.getInstrInfo();

    unsigned RegSize = getRegBitWidth(Reg, IGC);
    unsigned StoreSize = BytesToWrite ? BytesToWrite * 8 : RegSize;

    if (AArch64::GPR64RegClass.contains(Reg) ||
        AArch64::GPR64spRegClass.contains(Reg) ||
        AArch64::GPR32RegClass.contains(Reg)) {

      unsigned StoreOp;
      if (StoreSize == 64) {
        StoreOp = AArch64::STRXui;
      } else if (StoreSize == 32) {
        StoreOp = AArch64::STRWui;
      } else if (StoreSize == 16) {
        StoreOp = AArch64::STRHui;
      } else if (StoreSize == 8) {
        StoreOp = AArch64::STRBui;
      } else {
        snippy::fatal(
            formatv("Unsupported store size for GPR: {0} bits", StoreSize));
      }

      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(StoreOp))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }

    else if (AArch64::FPR8RegClass.contains(Reg)) {
      assert(StoreSize == 8 && "FPR8 requires 8-bit store");
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRBui))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (AArch64::FPR16RegClass.contains(Reg)) {
      assert(StoreSize == 16 && "FPR16 requires 16-bit store");
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRHui))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    // TODO: check FPR32_with_hsub_in_FPR16_loRegClass
    else if (AArch64::FPR32RegClass.contains(Reg)) {
      assert(StoreSize == 32 && "FPR32 requires 32-bit store");
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRSui))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (AArch64::FPR64RegClass.contains(Reg)) {
      assert(StoreSize == 64 && "FPR64 requires 64-bit store");
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRDui))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (AArch64::FPR128RegClass.contains(Reg)) {
      assert(StoreSize == 128 && "FPR128 requires 128-bit store");
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRQui))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    // Special registers
    else if (Reg == AArch64::NZCV) {
      auto &RI = State.getRegInfo();
      auto RP = IGC.pushRegPool();
      auto &RegClass = RI.getRegClass(AArch64::GPR64RegClassID);
      auto ScratchReg = getNonZeroReg("scratch reg for NZCV", RI, RegClass, *RP,
                                      MBB, AccessMaskBit::SupportRW);
      // Directly read NZCV value to temporary register and store it to stack
      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(AArch64::MRS))
          .addReg(ScratchReg)
          .addImm(AArch64SysReg::NZCV)
          .addReg(Reg); // TODO: check should I add side effects

      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STRXui))
          .addReg(ScratchReg)
          .addReg(AddrReg)
          .addImm(0);
    }
    // TODO: check support of other sysregs
    else if (AArch64::PPRRegClass.contains(Reg) ||
             AArch64::PPR_3bRegClass.contains(Reg) ||
             AArch64::PNRRegClass.contains(Reg)) {
      if (AArch64::PPRRegClass.contains(Reg) ||
          AArch64::PPR_3bRegClass.contains(Reg)) {
        getSupportInstBuilder(*this, MBB, Ins, Ctx,
                              InstrInfo.get(AArch64::STR_PXI))
            .addReg(Reg)
            .addReg(AddrReg)
            .addImm(0);
      } else if (AArch64::PNRRegClass.contains(Reg)) {
        getSupportInstBuilder(*this, MBB, Ins, Ctx,
                              InstrInfo.get(AArch64::STR_PXI))
            .addReg(Reg)
            .addReg(AddrReg)
            .addImm(0);
      }
    }

    else if (AArch64::ZPRRegClass.contains(Reg) ||
             AArch64::ZPR_4bRegClass.contains(Reg)) {
      getSupportInstBuilder(*this, MBB, Ins, Ctx,
                            InstrInfo.get(AArch64::STR_ZXI))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }

    else if (AArch64::MPRRegClass.contains(Reg)) {
      snippy::fatal("Matrix register storing unsupported now");
    }

    else {
      snippy::fatal(formatv(
          "Cannot generate store to memory for AArch64 register {0}", Reg));
    }
  }
  void generateReloadFromStack(
      InstructionGenerationContext &IGC, MCRegister Reg, MCRegister SP,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &MBB = IGC.MBB;
    assert(ProgCtx.stackEnabled() &&
           "An attempt to generate reload but stack was not enabled.");
    auto SpillSize = static_cast<int64_t>(
        getSpillSizeInBytes(Reg, ProgCtx, IGC.getSubtargetImpl()));

    if (ProgCtx.getConfig().StaticStack) {
      // When we don't have a stack pointer, we spill registers in
      // a statically allocated stack area for each function.
      auto &StaticStackCtx = ProgCtx.getStaticStack();
      if (!StaticStackCtx.isSPInReg())
        initRegWithSPAddr(IGC, Reg);
      StaticStackCtx.setSPAddrLocal(StaticStackCtx.getSPAddrLocal() +
                                    SpillSize);
      SP = StaticStackCtx.getRegWithSPAddrLocal();
    }
    auto &Ins = IGC.Ins;
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();
    loadRegFromAddrInReg(IGC, SP, Reg);
    getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(AArch64::ADDXri))
        .addDef(SP)
        .addReg(SP)
        .addImm(SpillSize)
        .addImm(0);
  }

  void generatePopNoReload(InstructionGenerationContext &IGC,
                           MCRegister Reg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getRegBitWidth(MCRegister Reg,
                          InstructionGenerationContext &IGC) const override {
    // TODO: check if all possible regs are covered...
    if (AArch64::GPR64RegClass.contains(Reg) ||
        AArch64::GPR64spRegClass.contains(Reg)) {
      return 64;
    }

    if (AArch64::GPR32RegClass.contains(Reg)) {
      return 32;
    }

    if (AArch64::FPR32RegClass.contains(Reg))
      return 32;
    if (AArch64::FPR64RegClass.contains(Reg))
      return 64;
    if (AArch64::FPR128RegClass.contains(Reg))
      return 128;

    return 64;
  }

  MCRegister regIndexToMCReg(InstructionGenerationContext &IGC, unsigned RegIdx,
                             RegStorageType Storage) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  RegStorageType regToStorage(Register Reg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned regToIndex(Register Reg) const override { SNIPPY_UNIMPLEMENTED(); }

  unsigned getNumRegs(RegStorageType Storage,
                      const TargetSubtargetInfo &SubTgt) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned
  getSpillSizeInBytes(MCRegister Reg, SnippyProgramContext &ProgCtx,
                      const TargetSubtargetInfo &SubTgt) const override {
    // TODO: make real spillsize in future
    return 16u;
  }

  std::vector<MCRegister>
  getCallerSavedRegs(const MachineFunction &MF,
                     ArrayRef<std::string> RegGroups) const override {
    // SNIPPY_UNIMPLEMENTED();
    // TODO: add registers here
    return {};
  }

  std::vector<std::string> getCallerSavedRegGroups() const override {
    // TODO: check it
    return {"X", "W", "D"};
  }

  std::vector<std::string> getCallerSavedLiveRegGroups() const override {
    // SNIPPY_UNIMPLEMENTED();
    return {};
  }

  unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                    const LLVMState &State) const override {
    // TODO: we can store register more efficient
    return 16u;
  }

  MachineInstr *
  generateMemoryBarrier(InstructionGenerationContext &IGC) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target, MDNode *MM,
                             std::optional<unsigned> OptOpcode,
                             MCRegister RA) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineInstr *generateTailCall(InstructionGenerationContext &IGC,
                                 const Function &Target) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineInstr *generateReturn(InstructionGenerationContext &IGC,
                               MCRegister RA) const override {
    auto &State = IGC.ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB =
        getSupportInstBuilder(*this, IGC.MBB, IGC.Ins,
                              IGC.MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(AArch64::RET))
            .addReg(RA);
    return MIB;
  }

  MachineInstr *generateNop(InstructionGenerationContext &IGC) const override {
    auto &State = IGC.ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB =
        getSupportInstBuilder(*this, IGC.MBB, IGC.Ins,
                              IGC.MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(AArch64::HINT))
            .addImm(0);
    return MIB;
  }

  unsigned getTransformSequenceLength(InstructionGenerationContext &IGC,
                                      APInt OldValue, APInt NewValue,
                                      MCRegister Register) const override {
    SNIPPY_UNIMPLEMENTED();
  }
  void transformValueInReg(InstructionGenerationContext &IGC, APInt OldValue,
                           APInt NewValue, MCRegister Register) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void loadEffectiveAddressInReg(InstructionGenerationContext &IGC,
                                 MCRegister Register, uint64_t BaseAddr,
                                 uint64_t Stride,
                                 MCRegister IndexReg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineOperand generateMemoryRelatedImmediate(
      const MCInstrDesc &InstrDesc, unsigned OperandIdx,
      const StridedImmediate &StridedImm, const SnippyProgramContext &ProgCtx,
      const CommonPolicyConfig &Cfg,
      ArrayRef<MachineOperand> PregeneratedOperands,
      MemAddr Addr) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineOperand
  generateTargetOperand(const MCInstrDesc &InstrDesc, unsigned OperandIdx,
                        const StridedImmediate &StridedImm,
                        const SnippyProgramContext &ProgCtx,
                        const CommonPolicyConfig &Cfg) const override {
    snippy::fatal("Requested generation for an unexpected operand type.");
  }

  unsigned getMaxInstrSize() const override { return 4u; }

  std::set<unsigned>
  getPossibleInstrsSize(const TargetSubtargetInfo &STI) const override {
    // NOTE: Thumb is only AArch32 extension
    return {4u};
  }

  bool isMultipleReg(Register Reg, const MCRegisterInfo &RI) const override {
    if (Reg == AArch64::NoRegister)
      return false;
    // If there is only one subreg in subregs,
    // then this register does not consist of smaller ones, which means it is
    // physical
    // TODO: check how X and W regs are encoded
    auto Subregs = RI.subregs_inclusive(Reg);
    return std::distance(Subregs.begin(), Subregs.end()) != 1;
  }

  bool isPhysRegClass(unsigned RegClassID,
                      const MCRegisterInfo &RI) const override {
    const auto &RC = RI.getRegClass(RegClassID);
    return std::all_of(RC.begin(), RC.end(), [this, &RI](unsigned Reg) {
      return !isMultipleReg(Reg, RI);
    });
  }

  Register getFirstPhysReg(Register Reg,
                           const MCRegisterInfo &RI) const override {
    if (Reg == AArch64::NoRegister)
      return Reg;
    auto Subregs = RI.subregs_inclusive(Reg);
    // TODO: rework phys registers mapping
    static_assert(
        AArch64::X0 < AArch64::X28 &&
        "The value of enum is expected to increase with increasing size of "
        "the GPR register");
    return *std::min_element(Subregs.begin(), Subregs.end());
  }

  void
  getSubregsInclusive(Register Reg, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    OutPhysRegs.clear();
    if (Reg == AArch64::NoRegister)
      return;
    llvm::append_range(OutPhysRegs, RI.subregs_inclusive(Reg));
  }

  void
  getPhysRegsFromUnit(Register RegUnit, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    OutPhysRegs.clear();
    if (RegUnit == AArch64::NoRegister)
      return;
    if (!isMultipleReg(RegUnit, RI)) {
      OutPhysRegs.push_back(RegUnit);
      return;
    }

    auto Subregs = RI.subregs_inclusive(RegUnit);
    copy_if(Subregs, std::back_inserter(OutPhysRegs),
            [this, &RI](auto &SubReg) { return !isMultipleReg(SubReg, RI); });
  }

  void getPhysRegsWithoutOverlaps(
      Register RegUnit, const MCRegisterInfo &RI,
      SmallVectorImpl<Register> &OutPhysRegs) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getMaxBranchDstMod(unsigned Opcode) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineBasicBlock *
  getBranchDestination(const MachineInstr &Branch) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool branchNeedsVerification(const MachineInstr &Branch) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool mayBeScheduled(const MachineInstr &MI) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineBasicBlock *generateBranch(InstructionGenerationContext &IGC,
                                    const MCInstrDesc &InstrDesc,
                                    MachineBasicBlock *Dst
  ) const override {
    SNIPPY_UNIMPLEMENTED();
  }


  MachineInstr &insertIndirectJump(InstructionGenerationContext &IGC,
                                   MachineBasicBlock &TBB,
                                   unsigned Opcode) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                   SnippyProgramContext &ProgCtx) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void insertFallbackBranch(MachineBasicBlock &From, MachineBasicBlock &To,
                            const LLVMState &State) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock &NewDestMBB) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock::iterator To) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                         MachineBasicBlock &OldDestMBB,
                         MachineBasicBlock &NewDestMBB) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void addTargetSpecificPasses(PassManagerWrapper &PM) const override {}

  void addTargetLegalizationPasses(PassManagerWrapper &PM) const override {
    // Expand pseudo MOVi{64,32}imm
    PM.add(createAArch64ExpandPseudoPass());
  }

  bool is64Bit(const TargetMachine &TM) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool isSelfcheckAllowed(const SnippyProgramContext &ProgCtx,
                          const SelfcheckConfig &SelfcheckCfg,
                          const MachineInstr &MI) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool isAtomicMemInstr(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool isVectorInstr(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  Error
  hasMandatoryTargetFeaturesForSMC(const MCSubtargetInfo &SI) const override {
    return createStringError(inconvertibleErrorCode(),
                             "SMC generation supported only for RISC-V");
  }

  bool isUnsupportedForSMC(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool
  canBeGeneratedAsCommonInstr(const MCInstrDesc &InstrDesc) const override {
    return true;
  }

  void getEncodedMCInstr(const MachineInstr *MI, const MCCodeEmitter &MCCE,
                         AsmPrinter &AP, const MCSubtargetInfo &STI,
                         SmallVector<char> &OutBuf) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  SmallVector<unsigned>
  getImmutableRegs(const MCRegisterClass &MCRegClass) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  const MCRegisterClass &
  getMCRegClassForBranch(SnippyProgramContext &ProgCtx,
                         const MachineInstr &Instr) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineInstr &
  updateLoopBranch(MachineInstr &Branch, const MCInstrDesc &InstrDesc,
                   ArrayRef<Register> ReservedRegs) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned
  getNumRegsForLoopBranch(const MCInstrDesc &BranchDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getLoopOverhead() const override { SNIPPY_UNIMPLEMENTED(); }

  unsigned getInstrSize(const MachineInstr &Inst,
                        LLVMState &State) const override {
    return 4u;
  }

  LoopType getLoopType(MachineInstr &Branch) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  LoopCounterInitResult insertLoopInit(InstructionGenerationContext &IGC,
                                       MachineInstr &Branch,
                                       const Branchegram &Branches,
                                       ArrayRef<Register> ReservedRegs,
                                       unsigned NIter) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  LoopCounterInsertionResult insertLoopCounter(
      InstructionGenerationContext &IGC, MachineInstr &Branch,
      ArrayRef<Register> ReservedRegs, unsigned NIter,
      RegToValueType &ExitingValues,
      const LoopCounterInitResult &CounterInitInfo) const override {
    SNIPPY_UNIMPLEMENTED();
  }


  virtual void initializeTargetPasses() const override {
    // auto *PM = PassRegistry::getPassRegistry();
    // TODO: implement AARch64 specific passes
  }

  unsigned countAddrsToGenerate(unsigned Opcode) const override {
    // TODO: Add memory instr generation
    // if (isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode) ||
    //   isLrInstr(Opcode) || isScInstr(Opcode))
    // return 1;
    return 0;
  }

  std::pair<AddressParts, MemAddresses>
  breakDownAddr(InstructionGenerationContext &IGC, AddressInfo AddrInfo,
                const MCInstrDesc &InstrDesc,
                MutableArrayRef<planning::PreselectedOpInfo> Preselected,
                unsigned AddrIdx,
                std::optional<MemAddr> MainPart = std::nullopt) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getWriteValueSequenceLength(InstructionGenerationContext &IGC,
                                       APInt Value,
                                       MCRegister Register) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  static void addGeneratedInstrsToBB(InstructionGenerationContext &IGC,
                                     ArrayRef<MCInst> Insts,
                                     const SnippyTarget &Tgt) {
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();

    for (const auto &Inst : Insts) {
      auto MIB = getSupportInstBuilder(Tgt, MBB, Ins, State.getCtx(),
                                       InstrInfo.get(Inst.getOpcode()));
      assert(Inst.begin()->isReg() &&
             "In write instructions, the first operand "
             "is always the destination register");
      MIB.addDef(Inst.begin()->getReg());
      llvm::for_each(llvm::drop_begin(Inst), [&MIB](const auto &Op) {
        if (Op.isReg())
          MIB.addReg(Op.getReg());
        else if (Op.isImm())
          MIB.addImm(Op.getImm());
        else
          llvm_unreachable("Unknown operand type");
      });
    }
  }
  void writeValueToReg(InstructionGenerationContext &IGC, APInt Value,
                       unsigned DstReg) const override {
    if (!isRegClassSupported(DstReg)) {
      snippy::fatal("Unsuppoerted register class, can`t store vcalue in it");
    }

    SmallVector<MCInst> InstrsForWrite;
    generateWriteValueSeq(IGC, Value, DstReg, InstrsForWrite);
    addGeneratedInstrsToBB(IGC, InstrsForWrite, *this);
  }
  void writeValueToCSR(InstructionGenerationContext &IGC, APInt Value,
                       unsigned DstReg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void copyRegToReg(InstructionGenerationContext &IGC, MCRegister Rs,
                    MCRegister Rd) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineInstr *loadSymbolAddress(InstructionGenerationContext &IGC,
                                  unsigned DestReg,
                                  const GlobalValue *Target) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MCInst generateLoadRegFromAddrInReg(InstructionGenerationContext &IGC,
                                      MCRegister AddrReg,
                                      MCRegister Reg) const {
    assert(AArch64::GPR64allRegClass.contains(AddrReg) &&
           "Expected address register be GPR64");
    if (AArch64::GPR32RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRWui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::GPR64allRegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRXui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::FPR8RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRBui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::FPR16RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRHui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::FPR32RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRSui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::FPR64RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRDui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }
    if (AArch64::FPR128RegClass.contains(Reg)) {
      return MCInstBuilder(AArch64::LDRQui)
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    }

    snippy::fatal(
        formatv("Cannot generate load from memory for register {0}", Reg));
    return {};
  }

  void generateLoadRegFromAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                               MCRegister Reg,
                               SmallVectorImpl<MCInst> &Insts) const {
    auto &MBB = IGC.MBB;
    auto &RP = IGC.getRegPool();
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(AArch64::GPR64RegClassID);
    auto XScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                     RP, MBB, AccessMaskBit::SupportRW);
    // Form address in scratch register.
    generateWriteValueSeq(IGC, APInt(getRegBitWidth(XScratchReg, IGC), Addr),
                          XScratchReg, Insts);
    Insts.push_back(generateLoadRegFromAddrInReg(IGC, XScratchReg, Reg));
  }

  void loadRegFromAddr(
      InstructionGenerationContext &IGC, uint64_t Addr, MCRegister Reg,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    SmallVector<MCInst> InstrsForWrite;
    generateLoadRegFromAddr(IGC, Addr, Reg, InstrsForWrite);
    addGeneratedInstrsToBB(IGC, InstrsForWrite, *this);
  }

  void loadRegFromAddrInReg(
      InstructionGenerationContext &IGC, MCRegister AddrReg, MCRegister Reg,
      SnippyMetadata MetadataMark = SnippyMetadata::Support) const override {
    const auto LoadInstr = generateLoadRegFromAddrInReg(IGC, AddrReg, Reg);
    addGeneratedInstrsToBB(IGC, {LoadInstr}, *this);
  }

  MCRegister
  getTmpRegisterForCheckSumSelfcheck(InstructionGenerationContext &IGC,
                                     const RegPoolWrapper &RP) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MCRegister generateInitRegisterValueForCheckSumSelfcheck(
      InstructionGenerationContext &IGC, MachineBasicBlock::iterator Ins,
      const RegPoolWrapper &RP, MCRegister Reg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void generateRegMove(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       LLVMContext &Context, const MCInstrInfo &InstrInfo,
                       MCRegister SrcReg, MCRegister DstReg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void generateCheckSumForSelfcheck(
      InstructionGenerationContext &IGC, MCRegister DstReg, MCRegister SrcReg,
      std::optional<MCRegister> TmpReg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void generateCheckForCheckSumSelfcheck(InstructionGenerationContext &IGC,
                                         MCRegister AccReg,
                                         MCRegister RefReg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void storeRegToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                      MCRegister Reg, unsigned BytesToWrite) const override {
    auto &MBB = IGC.MBB;
    auto RP = IGC.pushRegPool();
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(AArch64::GPR64RegClassID);
    RP->addReserved(getFirstPhysReg(Reg, RI), MBB);
    auto ScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                    *RP, MBB, AccessMaskBit::SupportRW);
    auto XRegBitSize = getRegBitWidth(ScratchReg, IGC);

    writeValueToReg(IGC, APInt(XRegBitSize, Addr), ScratchReg);
    storeRegToAddrInReg(IGC, ScratchReg, Reg, BytesToWrite);
  }

  void storeValueToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                        APInt Value) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void preselectAccessSizeOperand(
      InstructionGenerationContext &IGC, const MCInstrDesc &InstrDesc,
      MutableArrayRef<planning::PreselectedOpInfo> Preselected) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  AddressGenInfo selectAddrGenInfoForInstr(
      const SnippyProgramContext &ProgCtx, unsigned Opcode,
      const MachineBasicBlock &MBB,
      ArrayRef<planning::PreselectedOpInfo> Preselected = {}) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  void excludeFromMemRegsForInstr(
      const MCInstrDesc &InstrDesc, const MCRegisterInfo &RI,
      SmallVectorImpl<Register> &Regs,
      std::optional<MemAddr> Addr = std::nullopt,
      const CommonPolicyConfig *Cfg = nullptr) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::vector<Register> excludeRegsForOperand(InstructionGenerationContext &IGC,
                                              const MCRegisterClass &RC,
                                              const MCInstrDesc &InstrDesc,
                                              unsigned Operand) const override {
    return {};
  }

  std::vector<Register> includeRegs(unsigned Opcode,
                                    const MCRegisterClass &RC) const override {
    return {};
  }

  void reserveRegsIfNeeded(InstructionGenerationContext &IGC, unsigned Opcode,
                           bool isDst, bool isMem,
                           Register Reg) const override {
    // TODO: support register reservation
  }

  const TargetRegisterClass &getAddrRegClass() const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getAddrRegLen(const TargetMachine &TM) const override { return 64u; }

  bool canUseInBurstMode(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool canInitializeOperand(const MCInstrDesc &InstrDesc, unsigned OpIndex,
                            const LLVMState &State) const override {
    assert(InstrDesc.getNumOperands() > OpIndex &&
           "This must be the index of the operand");
    auto Operand = InstrDesc.operands()[OpIndex];
    // TODO: add more constraints in future
    if (Operand.OperandType != MCOI::OperandType::OPERAND_REGISTER)
      return false;
    return true;
  }

  StridedImmediate getImmOffsetRangeForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  unsigned getImmOffsetAlignmentForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  size_t getAccessSize(unsigned Opcode) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool isCall(unsigned Opcode) const override { return snippy::isCall(Opcode); }
  bool isSPRelative(unsigned Opcode) const override {
    return snippy::isSPRelative(Opcode);
  }

  void allocateMemoryInitializationRegs(
      InstructionGenerationContext &IGC,
      bool FollowCallingConvention) const override {
    SNIPPY_UNIMPLEMENTED();
  }
  std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const SnippyProgramContext &ProgCtx,
                     const MachineBasicBlock &MBB) const override {
    return {};
  }

  std::vector<MCRegister>
  getCalleeSavedRegs(const MCSubtargetInfo &SubTgt) const override {
    // TODO: reuse definitions from MCRegisterInfo
    return {AArch64::X19, AArch64::X20, AArch64::X21, AArch64::X22,
            AArch64::X23, AArch64::X24, AArch64::X25, AArch64::X26,
            AArch64::X27, AArch64::X28, AArch64::FP,  AArch64::LR,
            AArch64::D8,  AArch64::D9,  AArch64::D10, AArch64::D11,
            AArch64::D12, AArch64::D13, AArch64::D14, AArch64::D15};
  }

  std::vector<MCRegister> getGlobalStateRegs() const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool canProduceNaN(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  std::optional<std::pair<MCRegister, const MCRegisterClass *>>
  tryGetNaNRegisterAndClass(InstructionGenerationContext &InstrGenCtx,
                            MCRegister Reg) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool isFloatingPoint(MCRegister Reg) const override {
    // TODO: support fp instructions
    return false;
  }

  bool isFloatingPoint(const MCInstrDesc &InstrDesc) const override {
    LLVMContext Ctx;
    snippy::warn(WarningName::NotAWarning, Ctx, "Unsupported",
                 "AArch64 snippy target now does not support floating-point "
                 "instructions.");
    return false;
  }

  std::unique_ptr<AsmPrinter>
  createAsmPrinter(TargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const override {
    return std::make_unique<AArch64AsmPrinter>(TM, std::move(Streamer));
  }

  uint8_t getCodeAlignment(const TargetSubtargetInfo &STI) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineBasicBlock::iterator
  insertJumpThroughRelocation(InstructionGenerationContext &IGC,
                              uint64_t Addr) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  MachineBasicBlock::iterator generateJump(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Ins,
                                           MachineBasicBlock &TBB,
                                           LLVMState &State) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool fitsCondBranch(uint64_t Distance) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool fitsUncondBranch(uint64_t Distance) const override {
    SNIPPY_UNIMPLEMENTED();
  }


  void addAsmPrinterFlags(MachineInstr &MI) const override {}

  Error checkOperandsReinitializationForbidden(unsigned Opcode) const override {
    return Error::success();
  }

  size_t getNumImmOperands(const MCInstrDesc &InstrDesc) const override {
    SNIPPY_UNIMPLEMENTED();
  }

  bool shouldPreselectOperandInBurstMode(const MCInstrDesc &InstrDesc,
                                         unsigned OpIdx) const override {
    SNIPPY_UNIMPLEMENTED();
  }
}; // class SnippyAArch64Target

bool SnippyAArch64Target::matchesArch(Triple::ArchType Arch) const {
  return Arch == Triple::aarch64;
}

} // namespace

static SnippyTarget *getTheAArch64SnippyTarget() {
  static SnippyAArch64Target Target;
  return &Target;
}

void InitializeAArch64SnippyTarget() {
  SnippyTarget::registerTarget(getTheAArch64SnippyTarget());
}

} // namespace snippy
} // namespace llvm
