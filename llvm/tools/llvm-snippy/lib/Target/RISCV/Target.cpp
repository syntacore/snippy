//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/RegisterPool.h"

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/SerDesUtils.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "snippy/Simulator/Targets/RISCV.h"

#include "TargetConfig.h"
#include "TargetGenContext.h"

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "MCTargetDesc/RISCVInstPrinter.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "MCTargetDesc/RISCVMatInt.h"
#include "RISCV.h"
#include "RISCVAsmPrinter.h"
#include "RISCVGenerated.h"
#include "RISCVInstrInfo.h"
#include "RISCVRegisterInfo.h"
#include "RISCVSubtarget.h"
#include "TargetGenContext.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#include <cstdint>

#include <functional>
#include <numeric>
#include <variant>

#define DEBUG_TYPE "snippy-riscv"

#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"

// include computeAvailableFeatures and computeRequiredFeatures.
#define GET_COMPUTE_FEATURES
#include "RISCVGenInstrInfo.inc"
#undef GET_COMPUTE_FEATURES

namespace llvm {
#define GET_REGISTER_MATCHER
#include "RISCVGenAsmMatcher.inc"
namespace snippy {

enum class RVVModeChangeMode {
  MC_ANY,
  MC_VSETIVLI,
  MC_VSETVLI,
  MC_VSETVL,
};

enum class LRSCGenMode { MaySucceed, MustFail };

llvm::cl::OptionCategory
    SnippyRISCVOptions("llvm-snippy RISCV-specific options");

static snippy::opt<bool> UseSplatsForRVVInit(
    "riscv-use-splats-for-rvv-init",
    cl::desc("initialize vector registers using splats instead of slides"),
    cl::cat(SnippyRISCVOptions), cl::init(false));

static snippy::opt<std::string> DumpRVVConfigurationInfo(
    "riscv-dump-rvv-config",
    cl::desc("Print verbose information about selected RVV configurations"),
    cl::init(""), cl::ValueOptional, cl::cat(SnippyRISCVOptions));

static snippy::opt<bool>
    RISCVDisableMisaligned("riscv-disable-misaligned-access",
                           cl::desc("disable misaligned load/store generation"),
                           cl::cat(SnippyRISCVOptions), cl::init(false));

cl::alias UseSplatsForRVVInitAlias(
    "snippy-riscv-use-splats-for-rvv-init",
    cl::desc("Alias for -riscv-use-splats-for-rvv-init"),
    cl::aliasopt(UseSplatsForRVVInit));

cl::alias
    DumpConfigurationInfoAlias("snippy-riscv-dump-rvv-config",
                               cl::desc("Alias for -riscv-dump-rvv-config"),
                               cl::aliasopt(DumpRVVConfigurationInfo));

static snippy::opt<bool> InitVRegsFromMemory(
    "riscv-init-vregs-from-memory",
    cl::desc("use preinitialized memory for initializing vector registers"),
    cl::cat(SnippyRISCVOptions));

static snippy::opt<bool>
    NoMaskModeForRVV("riscv-nomask-mode-for-rvv",
                     cl::desc("force use nomask for vector instructions"),
                     cl::Hidden, cl::cat(SnippyRISCVOptions), cl::init(false));

static snippy::opt<bool>
    SelfCheckRVV("enable-selfcheck-rvv",
                 cl::desc("turning on selfcheck for rvv instructions"),
                 cl::Hidden, cl::init(false));

struct LRSCGenModeEnumOption
    : public snippy::EnumOptionMixin<LRSCGenModeEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(
        LRSCGenMode::MaySucceed, "may-succeed",
        "Generate constained LR and SC pairs in a way that they may "
        "eventually succeed on all spec-compliant implementations");
    Mapper.enumCase(LRSCGenMode::MustFail, "must-fail",
                    "Generated SC will always fail on each spec-compliant "
                    "implementation with the same or smaller reservation set "
                    "size (see riscv-reservation-set-size)");
  }
};

struct RVVModeChangeEnumOption
    : public snippy::EnumOptionMixin<RVVModeChangeEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(RVVModeChangeMode::MC_ANY, "any", "anything goes");
    Mapper.enumCase(RVVModeChangeMode::MC_VSETIVLI, "vsetivli",
                    "prefer vsetivli");
    Mapper.enumCase(RVVModeChangeMode::MC_VSETVLI, "vsetvli", "prefer vsetvli");
    Mapper.enumCase(RVVModeChangeMode::MC_VSETVL, "vsetvl", "prefer vsetvl");
  }
};

static snippy::opt<RVVModeChangeMode> RVVModeChangePreferenceOpt(
    "riscv-preference-for-rvv-mode-change",
    cl::desc("Preferences for RVV mode chaning instructions (debug only)"),
    RVVModeChangeEnumOption::getClValues(), cl::Hidden,
    cl::init(RVVModeChangeMode::MC_ANY), cl::cat(SnippyRISCVOptions));

} // namespace snippy

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::RVVModeChangeMode,
                                           snippy::RVVModeChangeEnumOption)

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::LRSCGenMode,
                                           snippy::LRSCGenModeEnumOption)

namespace snippy {

static snippy::opt<size_t> ReservationSetSize(
    "riscv-reservation-set-size",
    cl::desc("Set the maximum reservation set size in bytes. It is expected "
             "that each reservation set is aligned on the provided size."),
    cl::init(64), cl::cat(SnippyRISCVOptions));

namespace {

struct SelfcheckDestRegistersInfo {
  Register BaseDestRegister;
  unsigned NumRegs;

  SelfcheckDestRegistersInfo(Register BaseDestReg, unsigned Num)
      : BaseDestRegister(BaseDestReg), NumRegs(Num) {}
};

// Gap is a distance between segments for segment loads
struct ElemWidthAndGapForVectorLoad {
  unsigned ElemWidth;
  unsigned Gap;
};

ElemWidthAndGapForVectorLoad
getElemWidthAndGapForVectorLoad(unsigned Opcode, unsigned EEW,
                                const MachineBasicBlock &MBB,
                                const GeneratorContext &GenCtx) {
  assert(EEW && "Effective element width can not be zero");
  const auto &RISCVCtx =
      GenCtx.getTargetContext().getImpl<RISCVGeneratorContext>();

  if (isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) {
    // segment and non-indexed loads
    auto SEW = static_cast<unsigned>(RISCVCtx.getSEW(MBB));
    auto [EMUL, IsFractionEMUL] =
        computeDecodedEMUL(SEW, EEW, RISCVCtx.getLMUL(MBB));
    assert(RISCVVType::isValidLMUL(EMUL, IsFractionEMUL));
    return {EEW, EMUL};
  }

  auto [LMUL, IsFractional] = RISCVVType::decodeVLMUL(RISCVCtx.getLMUL(MBB));
  assert(RISCVVType::isValidLMUL(LMUL, IsFractional));

  if (isRVVIndexedSegLoadStore(Opcode))
    return IsFractional ? ElemWidthAndGapForVectorLoad{EEW, 1}
                        : ElemWidthAndGapForVectorLoad{EEW, LMUL};

  return {EEW, 0};
}

bool isDefaultSelfcheckEnough(unsigned Opcode) {
  if (!isRVV(Opcode) || isRVVModeSwitch(Opcode) || isRVVScalarMove(Opcode))
    return true;
  switch (Opcode) {
  case RISCV::VCPOP_M:
  case RISCV::VFIRST_M:
    return true;
  }
  return false;
}

std::vector<SelfcheckDestRegistersInfo>
getInfoAboutRegsForSelfcheck(unsigned Opcode, Register FirstDestReg,
                             const MachineBasicBlock &MBB,
                             const GeneratorContext &GenCtx) {
  // Not RVV instruction isn't target specific fot selfcheck,
  // so we provide only one destination register to store
  std::vector<SelfcheckDestRegistersInfo> SelfcheckSegsInfo;
  if (isDefaultSelfcheckEnough(Opcode)) {
    SelfcheckSegsInfo.emplace_back(FirstDestReg, 1);
    return SelfcheckSegsInfo;
  }
  // Whole register moves (vmv<nr>r.v) and loads (vl<nr>r) affects
  // <nr> registers as destination of operation. So, for selfcheck
  // we need to store <nr> consecutive registers starting from the
  // base one
  if (isRVVWholeRegisterInstr(Opcode)) {
    auto DestRegsNum = getRVVWholeRegisterCount(Opcode);
    assert(DestRegsNum);
    FirstDestReg = DestRegsNum > 1 ? getBaseRegisterForVRMClass(FirstDestReg)
                                   : FirstDestReg;
    SelfcheckSegsInfo.emplace_back(FirstDestReg, DestRegsNum);
    return SelfcheckSegsInfo;
  }

  const auto &RISCVCtx =
      GenCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto SEW = static_cast<unsigned>(RISCVCtx.getSEW(MBB));
  auto EEW = getDataElementWidth(Opcode, SEW) * CHAR_BIT;
  assert(RISCVVType::isValidSEW(EEW));

  auto VL = RISCVCtx.getVL(MBB);
  auto VLEN = RISCVCtx.getVLEN();

  auto addSelfcheckDestRegistersInfo =
      [&SelfcheckSegsInfo, VL, VLEN](unsigned ElemWidth, Register DestReg) {
        // A commom formula for calculating the number of consecutive
        // destination registers in one segment for vector load
        auto DestRegsNum = std::ceil(double(VL * ElemWidth) / VLEN);
        SelfcheckSegsInfo.emplace_back(DestReg, DestRegsNum);
      };

  auto &&[ElemWidth, Gap] =
      getElemWidthAndGapForVectorLoad(Opcode, EEW, MBB, GenCtx);

  // Gap == 0 --> non-segment instruction
  if (!Gap) {
    addSelfcheckDestRegistersInfo(ElemWidth, FirstDestReg);
    return SelfcheckSegsInfo;
  }

  auto NumFields = getNumFields(Opcode);
  for (auto I = 0u; I < NumFields; ++I) {
    addSelfcheckDestRegistersInfo(ElemWidth, FirstDestReg);
    FirstDestReg = Register(FirstDestReg.id() + Gap);
  }
  return SelfcheckSegsInfo;
}

bool isCompressedBranch(unsigned Opcode) {
  return Opcode == RISCV::C_BEQZ || Opcode == RISCV::C_BNEZ;
}

bool isEqBranch(unsigned Opcode) {
  return Opcode == RISCV::BEQ || Opcode == RISCV::C_BEQZ;
}

unsigned hasNonZeroRegAvailable(const MCRegisterClass &RegClass,
                                const RegPoolWrapper &RP,
                                AccessMaskBit Mask = AccessMaskBit::RW) {
  return RP.getNumAvailable(
      RegClass, /*Filter*/ [](unsigned Reg) { return Reg == RISCV::X0; }, Mask);
}

unsigned getNonZeroReg(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
                       AccessMaskBit Mask = AccessMaskBit::RW) {
  return RP.getAvailableRegister(
      Desc, RI, RegClass, MBB,
      /*Filter*/ [](unsigned Reg) { return Reg == RISCV::X0; }, Mask);
}

static bool isSupportedLoadStore(unsigned Opcode) {
  return isLoadStore(Opcode) || isCLoadStore(Opcode) || isFPLoadStore(Opcode) ||
         isCFPLoadStore(Opcode) || isRVVUnitStrideLoadStore(Opcode) ||
         isRVVStridedLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
         isRVVIndexedLoadStore(Opcode) || isRVVUnitStrideSegLoadStore(Opcode) ||
         isRVVStridedSegLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode) ||
         isRVVWholeRegLoadStore(Opcode) || isRVVUnitStrideMaskLoadStore(Opcode);
}

static MCRegister regIndexToMCReg(unsigned RegIdx, RegStorageType Storage,
                                  bool hasDExt) {
  switch (Storage) {
  case RegStorageType::XReg:
    return RISCV::X0 + RegIdx;
  case RegStorageType::FReg:
    return hasDExt ? (RISCV::F0_D + RegIdx) : (RISCV::F0_F + RegIdx);
  case RegStorageType::VReg:
    return RISCV::V0 + RegIdx;
  }
  llvm_unreachable("Unknown storage type");
}

static RegStorageType regToStorage(Register Reg) {
  if (RISCV::X0 <= Reg && Reg <= RISCV::X31)
    return RegStorageType::XReg;
  if ((RISCV::F0_D <= Reg && Reg <= RISCV::F31_D) ||
      (RISCV::F0_F <= Reg && Reg <= RISCV::F31_F) ||
      (RISCV::F0_H <= Reg && Reg <= RISCV::F31_H))
    return RegStorageType::FReg;
  assert(RISCV::V0 <= Reg && Reg <= RISCV::V31 && "unknown register");
  return RegStorageType::VReg;
}

static bool isLegalRVVInstr(unsigned Opcode, const RVVConfiguration &Cfg) {
  if (!isRVV(Opcode))
    return false;

  auto SEW = Cfg.SEW;
  auto LMUL = Cfg.LMUL;

  if (!Cfg.IsLegal) {
    // From RISCV-V spec 1.0:
    //    vset{i}vl{i} and whole-register loads, stores, and moves do not depend
    //    upon vtype.
    // This means that they are legal even with an illegal configurations
    if (isRVVModeSwitch(Opcode) || isRVVWholeRegLoadStore(Opcode) ||
        isRVVWholeRegisterMove(Opcode))
      return true;
    return false;
  }
  // RVV loads and stores have an element width encoded in the opcode:
  // unit-stride and strided instructions encode element width, whereas indexed
  // encode index element width. It means that element width (EEW) may differ
  // from the SEW from VType. In this cases actual LMUL (called EMUL) does not
  // match the LMUL from VTYPE but it still must be bounds that are 1/8 <= EMUL
  // <= 8, EMUL = EEW / SEW * LMUL.
  if (isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
      isRVVStridedLoadStore(Opcode)) {
    // EEW is a data element width.
    auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
    return isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
  }
  if (isRVVIndexedLoadStore(Opcode)) {
    // EEW is an index element width.
    auto EEW = getIndexElementWidth(Opcode);
    return isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
  }
  // RVV segment loads/stores encodes not only EEW in the opcode, but number of
  // fields (NFIELDS) as well. This introduces one more restriction in addition
  // to the previous one on EMUL: EMUL * NFIELDS <= 8. Note: EMUL from the
  // previous inequality is a EMUL of data elements. So, for segment unit-stride
  // and strided instructions we should check:
  //   1/8 <= EMUL <= 8 and EMUL * NFIELDS <= 8.
  // But for segment indexed instructions we must check:
  //   1/8 <= EMUL <= 8 and LMUL * NFIELDS <= 8, because data multiplier is LMUL
  //   from the current VType.
  if (isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) {
    // EEW is a data element width.
    auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
    if (!isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL))
      return false;
    auto EMUL = computeEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(EMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVIndexedSegLoadStore(Opcode)) {
    auto EEW = getIndexElementWidth(Opcode);
    if (!isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL))
      return false;
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVFloatingPoint(Opcode) && static_cast<unsigned>(SEW) < 32u) {
    // If the EEW of a vector floating-point operand does not correspond to a
    // supported IEEE floating-point type, the instruction encoding is reserved.
    // Half-precision is unsupported now.
    return false;
  }
  if (isRVVExt(Opcode)) {
    // If the source EEW is not a supported width, or source EMUL would be below
    // the minimum legal LMUL, the instruction encoding is reserved.
    auto Factor = getRVVExtFactor(Opcode);
    assert(Factor <= static_cast<unsigned>(SEW));
    assert(static_cast<unsigned>(SEW) % Factor == 0);
    auto EEW = static_cast<unsigned>(SEW) / Factor;
    if (!isLegalSEW(EEW))
      return false;
    return isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
  }
  if (isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
      isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) {
    // Both widening and narrowing instructions use operands with EEW = SEW * 2.
    auto EEW = static_cast<unsigned>(SEW) * 2u;
    if (!isLegalSEW(EEW))
      return false;
    // Check that LMUL * 2 is also legal.
    return isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
  }
  if (isRVVGather16(Opcode)) {
    // The vrgatherei16.vv form uses SEW/LMUL for the data in vs2 but EEW=16 and
    // EMUL = (16/SEW)*LMUL for the indices in vs1.
    auto EEW = 16u;
    return isValidEMUL(static_cast<unsigned>(SEW), EEW, LMUL);
  }

  return true;
}

RISCVMatInt::InstSeq getIntMatInstrSeq(APInt Value, GeneratorContext &GC) {
  [[maybe_unused]] const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  assert((ST.getXLen() == 64 && Value.getBitWidth() == 64) ||
         (ST.getXLen() == 32 && isInt<32>(Value.getSExtValue())));

  return RISCVMatInt::generateInstSeq(
      Value.getSExtValue(),
      GC.getLLVMState().getSubtargetInfo().getFeatureBits());
}

void generateRVVMaskReset(const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator Ins) {
  if (NoMaskModeForRVV)
    return;
  getSupportInstBuilder(MBB, Ins, MBB.getParent()->getFunction().getContext(),
                        InstrInfo.get(RISCV::VMXNOR_MM))
      .addReg(RISCV::V0, RegState::Define)
      .addReg(RISCV::V0, RegState::Undef)
      .addReg(RISCV::V0, RegState::Undef);
}

// This function creates configuration with all ones value in Mask
static std::tuple<RVVConfiguration, RVVConfigurationInfo::VLVM>
constructRVVModeWithVMReset(RISCVII::VLMUL LMUL, unsigned VL, unsigned SEW,
                            bool TailAgnostic, bool MaskAgnostic) {
  RVVConfiguration Config{/* IsLegal */ true,
                          LMUL,
                          static_cast<RVVConfiguration::VSEW>(SEW),
                          MaskAgnostic,
                          TailAgnostic,
                          RVVConfiguration::VXRMMode::RNU,
                          /* VxsatEnable */ false};

  RVVConfigurationInfo::VLVM VLVM{VL, APInt::getMaxValue(VL)};
  return {Config, VLVM};
}

static unsigned getMemOperandIdx(const MCInstrDesc &InstrDesc) {
  auto Opcode = InstrDesc.getOpcode();
  assert(isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode) ||
         isLrInstr(Opcode) || isScInstr(Opcode));
  auto MemMCOpInfo = std::find_if(
      InstrDesc.operands().begin(), InstrDesc.operands().end(),
      [](const auto &OpInfo) {
        return OpInfo.OperandType == MCOI::OperandType::OPERAND_MEMORY;
      });
  assert(MemMCOpInfo != InstrDesc.operands().end());
  return std::distance(InstrDesc.operands().begin(), MemMCOpInfo);
}

static const MachineOperand &getMemOperand(const MachineInstr &MI) {
  const auto &InstrDesc = MI.getDesc();
  unsigned Idx = getMemOperandIdx(InstrDesc);
  const auto &MemOp = MI.getOperand(Idx);
  assert(MemOp.isReg() && "Memory operand is expected to be a register");
  return MemOp;
}

static uint64_t uintToTargetXLen(bool Is64Bit, uint64_t Value) {
  return Is64Bit ? Value : static_cast<uint32_t>(Value);
}

static MemAddresses generateStridedMemAccesses(MemAddr Base,
                                               int long long Stride, size_t N,
                                               bool Is64Bit) {
  MemAddresses Addresses(N);
  std::generate(Addresses.begin(), Addresses.end(),
                [Base, Stride, Is64Bit, i = 0]() mutable {
                  return uintToTargetXLen(Is64Bit, Base + Stride * i++);
                });
  return Addresses;
}

static std::pair<AddressParts, MemAddresses>
breakDownAddrForRVVStrided(AddressInfo AddrInfo, const MachineInstr &MI,
                           GeneratorContext &GC, bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isRVVStridedLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode));

  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto VL = TgtCtx.getVL(*MI.getParent());
  const auto &AddrReg = getMemOperand(MI);
  auto AddrRegIdx = MI.getOperandNo(&AddrReg);
  auto AddrValue = AddrInfo.Address;
  if (VL == 0)
    // When VL is zero, we may leave any values in address base and stride
    // registers.
    return std::make_pair(AddressParts{}, MemAddresses{});
  auto &State = GC.getLLVMState();
  auto &RI = State.getRegInfo();
  AddressPart MainPart{AddrReg, APInt(ST.getXLen(), AddrValue), RI};

  if (VL == 1)
    // When VL is one, we may leave any value in the stride register.
    return std::make_pair(AddressParts{std::move(MainPart)},
                          MemAddresses{uintToTargetXLen(Is64Bit, AddrValue)});

  // Stride operand must be the next after the addr reg.
  assert((MI.getNumOperands() > AddrRegIdx + 1) && "Expected stride operand");
  const auto &StrideReg = MI.getOperand(AddrRegIdx + 1);
  assert(StrideReg.isReg() && "Stride operand must be reg");
  assert(StrideReg.getReg() != AddrReg.getReg() &&
         "Stride and addr regs cannot match");
  assert(AddrInfo.MinStride >= 1);
  if (StrideReg.getReg() == RISCV::X0) {
    // We cannot write to X0. No additional randomization is possible.
    MemAddresses Addresses(
        VL, uintToTargetXLen(Is64Bit, MainPart.Value.getZExtValue()));
    return std::make_pair(AddressParts{std::move(MainPart)},
                          std::move(Addresses));
  }

  // Try to choose a stride different from zero. To do that we find the
  // maximum possible stride for the given AddrInfo and randomize it in
  // [MinStride, MaxStride]. The chosen stride must not contradict memory
  // scheme (stride % AddrInfo.MinStride == 0). When Stride is 0, AccessSize
  // equals element size.
  // The maximum size we can read is
  //    `MaxOffset = (VL - 1) * MaxStride`
  // => `MaxStride = MaxOffset / (VL - 1)`
  // We can choose any stride when VL is one.
  assert(VL > 1 && "Cases for VL = 0 and 1 must have been processed above");
  auto MaxStride = AddrInfo.MaxOffset / (VL - 1);
  // We will randomize the stride multiplier to exclude illegal strides.
  int long long MaxStrideMultiplier = MaxStride / AddrInfo.MinStride;
  auto StrideMultiplier =
      RandEngine::genInInterval(-MaxStrideMultiplier, MaxStrideMultiplier);
  auto Stride =
      StrideMultiplier * static_cast<int long long>(AddrInfo.MinStride);
  if (Stride < 0) {
    // When Stride is negative, change starting position such that last read
    // will start at the original AddrValue. It means that we must start
    // reading from the last element. `(VL - 1) * |Stride|`
    // calculates the last element offset.
    AddrValue += (VL - 1) * (-Stride);
    MainPart.Value = APInt(ST.getXLen(), AddrValue);
  }

  AddressPart StridePart{StrideReg, APInt(ST.getXLen(), Stride), RI};
  AddressParts Parts = {std::move(MainPart), std::move(StridePart)};

  auto Addresses = generateStridedMemAccesses(MainPart.Value.getZExtValue(),
                                              Stride, VL, Is64Bit);

  return std::make_pair(std::move(Parts), std::move(Addresses));
}

static std::pair<AddressParts, MemAddresses>
breakDownAddrForRVVIndexed(AddressInfo AddrInfo, const MachineInstr &MI,
                           GeneratorContext &GC, bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode));

  auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  unsigned VL = TgtCtx.getVL(*MI.getParent());
  if (VL == 0)
    // When VL is zero we may leave any values in address base and index
    // registers.
    return std::make_pair(AddressParts{}, MemAddresses{});

  const auto &AddrReg = getMemOperand(MI);
  auto AddrRegIdx = MI.getOperandNo(&AddrReg);
  assert((MI.getNumOperands() > AddrRegIdx + 1) && "Expected index operand");
  const auto &IdxOp = MI.getOperand(AddrRegIdx + 1);
  assert(IdxOp.isReg() && "Index operand must have register type");
  unsigned IdxReg = IdxOp.getReg();
  assert(RISCV::VRRegClass.contains(IdxReg) && "Index operand must be vreg");
  // EEW of index element.
  auto EIEW = getIndexElementWidth(Opcode);

  // Check that MinOffset and MaxOffset are already aligned to MinStride
  assert(AddrInfo.MinOffset % AddrInfo.MinStride == 0);
  assert(AddrInfo.MaxOffset % AddrInfo.MinStride == 0);

  // For indexed loads/stores, only base address + index must be legal according
  // to the memory scheme, but base address by itself doesn't have to be. So the
  // base address can be offset by some amount, as long as the final access is
  // still legal.

  uint64_t IndexMaxValue = APInt::getMaxValue(EIEW).getZExtValue();

  // Start by generating an offset for the base address.
  // The minimum value of the base address's offset is such that after adding
  // the maximum possible offset that can stored in the index register to the
  // final base address, the access is still legal according to the memory
  // scheme
  int64_t MinBaseOffset = AddrInfo.MinOffset - IndexMaxValue;
  // The maximum value is such that after adding an offset of 0 stored in the
  // index register to the final base address, the access is still legal
  // according to the memory scheme.
  int64_t MaxBaseOffset = AddrInfo.MaxOffset;
  // Offset the base address
  auto BaseOffset = RandEngine::genInInterval(MinBaseOffset, MaxBaseOffset);
  uint64_t BaseAddr = AddrInfo.Address + BaseOffset;
  // Align base address
  int64_t NextAlignedOffset = (BaseOffset < 0)
                                  ? -alignDown(-BaseOffset, AddrInfo.MinStride)
                                  : alignTo(BaseOffset, AddrInfo.MinStride);
  // Take into account that it might be less then the minimum allowed offset
  NextAlignedOffset = std::max<int64_t>(NextAlignedOffset, AddrInfo.MinOffset);
  // Calculate the minimum value that the index register must hold so that the
  // final access is properly aligned
  uint64_t IndexMinValue = NextAlignedOffset - BaseOffset;
  assert((BaseOffset + IndexMinValue) % AddrInfo.MinStride == 0);
  // Add a multiple of stride
  IndexMaxValue = std::min<uint64_t>(IndexMaxValue, MaxBaseOffset - BaseOffset);
  assert(IndexMaxValue >= IndexMinValue);
  auto MaxN = (IndexMaxValue - IndexMinValue) / AddrInfo.MinStride;

  AddressPart MainPart{AddrReg, APInt(ST.getXLen(), BaseAddr)};

  AddressParts ValueToReg = {MainPart};
  MemAddresses Addresses;
  auto VLEN = TgtCtx.getVLEN();
  unsigned NIdxsPerVReg = VLEN / EIEW;
  while (VL > 0) {
    // Get the number of indices we can write into one VReg. One VReg can hold
    // no more than NIdxsPerVReg. So, we might need to write not a single
    // register but a group.
    auto NElts = std::min(VL, NIdxsPerVReg);
    auto Offsets = APInt::getZero(VLEN);
    for (size_t ElemIdx = 0; ElemIdx < NElts; ++ElemIdx) {
      // TODO: for unordered stores, generating indices like this is not
      // correct, since store order for overlapping regions is not defined
      auto N = RandEngine::genInInterval(MaxN);
      auto IndexValue = IndexMinValue + AddrInfo.MinStride * N;
      Offsets.insertBits(IndexValue, ElemIdx * EIEW, EIEW);

      // Verify that generated address is legal according to memory scheme
      [[maybe_unused]] int64_t FinalOffset =
          BaseAddr + IndexValue - AddrInfo.Address;
      assert(FinalOffset >= AddrInfo.MinOffset);
      assert(FinalOffset <= AddrInfo.MaxOffset);
      assert(FinalOffset % AddrInfo.MinStride == 0);
      Addresses.push_back(uintToTargetXLen(Is64Bit, BaseAddr + IndexValue));
    }
    VL -= NElts;
    ValueToReg.emplace_back(IdxReg++, Offsets);
  }

  return std::make_pair(std::move(ValueToReg), std::move(Addresses));
}

static std::pair<AddressParts, MemAddresses>
breakDownAddrForInstrWithImmOffset(AddressInfo AddrInfo, const MachineInstr &MI,
                                   GeneratorContext &GC, bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isLoadStore(Opcode) || isCLoadStore(Opcode) || isFPLoadStore(Opcode) ||
         isCFPLoadStore(Opcode));

  auto &State = GC.getLLVMState();
  auto &RI = State.getRegInfo();
  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  const auto &AddrReg = getMemOperand(MI);
  auto AddrRegIdx = MI.getOperandNo(&AddrReg);
  // Offset operand must be the next after the addr reg.
  assert((MI.getNumOperands() > AddrRegIdx + 1) && "Expected offset operand");
  const auto &AddrImm = MI.getOperand(AddrRegIdx + 1);
  assert(AddrImm.isImm() && "Offset operand must be imm");
  auto AddrValue = AddrInfo.Address - AddrImm.getImm();

  auto Part = AddressPart{AddrReg, APInt(ST.getXLen(), AddrValue), RI};
  return std::make_pair<AddressParts, MemAddresses>(
      {std::move(Part)}, {uintToTargetXLen(Is64Bit, AddrInfo.Address)});
}

using OpcodeFilter = GeneratorContext::OpcodeFilter;

static OpcodeFilter
getDefaultPolicyFilter(const MachineBasicBlock &MBB,
                       const RISCVGeneratorContext &RISCVCtx) {
  if (!RISCVCtx.hasActiveRVVMode(MBB))
    return [](unsigned Opcode) {
      if (isRVV(Opcode) && !isRVVModeSwitch(Opcode))
        return false;
      return true;
    };

  return [&RISCVCtx, &MBB](unsigned Opcode) {
    if (isRVV(Opcode) &&
        !isLegalRVVInstr(Opcode, RISCVCtx.getCurrentRVVCfg(MBB)))
      return false;
    return true;
  };
}

inline bool checkSupportedOrdering(const OpcodeHistogram &H) {
  if (H.weight(RISCV::LR_W_RL) != 0 || H.weight(RISCV::SC_W_AQ) != 0 ||
      H.weight(RISCV::LR_D_RL) != 0 || H.weight(RISCV::SC_D_AQ) != 0)
    return false;
  return true;
}

/// Helper function to calculate load/store alignment based on whether
/// misaligned access is enabled or not
static size_t getLoadStoreAlignment(unsigned Opcode, unsigned SEW = 0) {
  if (RISCVDisableMisaligned)
    return getLoadStoreNaturalAlignment(Opcode, SEW);
  return 1;
}

template <typename It> static void storeWordToMem(It MemIt, uint32_t Value) {
  auto RegAsBytes = std::vector<uint8_t>{};
  convertNumberToBytesArray(Value, std::back_inserter(RegAsBytes));
  std::copy(RegAsBytes.rbegin(), RegAsBytes.rend(), MemIt);
}

class SnippyRISCVTarget final : public SnippyTarget {

public:
  SnippyRISCVTarget() {
    // TODO: use model interface to fetch restricted sections

    // htif
    ReservedRanges.emplace_back(0, 0xFFF1001000, 8, 0xFFF1001000, "rw");
    // clint
    ReservedRanges.emplace_back(0, 0xFFF1000000, 8, 0xFFF1000000, "rwx");
  }

  std::unique_ptr<TargetGenContextInterface>
  createTargetContext(const GeneratorContext &Ctx) const override;

  std::unique_ptr<TargetConfigInterface> createTargetConfig() const override;

  std::unique_ptr<SimulatorInterface>
  createSimulator(llvm::snippy::DynamicLibrary &ModelLib,
                  const SimulationConfig &Cfg,
                  const TargetGenContextInterface *TgtGenCtx,
                  RVMCallbackHandler *CallbackHandler,
                  const TargetSubtargetInfo &Subtarget) const override;

  bool matchesArch(Triple::ArchType Arch) const override {
    return Arch == Triple::riscv32 || Arch == Triple::riscv64;
  }

  SectionDesc const *
  touchesReservedRegion(SectionDesc const &desc) const override {
    auto Touches =
        std::find_if(ReservedRanges.begin(), ReservedRanges.end(),
                     [&desc](auto &Range) { return Range.interfere(desc); });
    if (Touches != ReservedRanges.end())
      return &*Touches;
    else
      return nullptr;
  }
  bool checkOpcodeSupported(int Opcode,
                            const MCSubtargetInfo &SI) const override {
    auto Features = SI.getFeatureBits();
    FeatureBitset AvailableFeatures =
        RISCV_MC::computeAvailableFeatures(Features);
    FeatureBitset RequiredFeatures = RISCV_MC::computeRequiredFeatures(Opcode);
    FeatureBitset MissingFeatures =
        (AvailableFeatures & RequiredFeatures) ^ RequiredFeatures;
    return MissingFeatures.none();
  }

  std::unique_ptr<IRegisterState>
  createRegisterState(const TargetSubtargetInfo &ST) const override {
    const auto &RST = static_cast<const RISCVSubtarget &>(ST);
    return std::make_unique<RISCVRegisterState>(RST);
  }

  bool needsGenerationPolicySwitch(unsigned Opcode) const override {
    return isRVVModeSwitch(Opcode);
  }

  // This function differs from the RISCVVType::decodeVLMUL in that it also
  // handles the reserved LMUL
  static std::pair<unsigned, bool> decodeVLMUL(RISCVII::VLMUL LMUL) {
    if (LMUL == RISCVII::VLMUL::LMUL_RESERVED)
      return std::make_pair(1 << static_cast<unsigned>(LMUL), false);
    return RISCVVType::decodeVLMUL(LMUL);
  }

  std::vector<Register>
  getRegsForSelfcheck(const MachineInstr &MI, const MachineBasicBlock &MBB,
                      const GeneratorContext &GenCtx) const override {
    const auto &FirstDestOperand = MI.getOperand(0);
    assert(FirstDestOperand.isReg());

    auto &&SelfcheckSegsInfo = getInfoAboutRegsForSelfcheck(
        MI.getOpcode(), FirstDestOperand.getReg(), MBB, GenCtx);

    std::vector<Register> Regs;
    for (const auto &SelfcheckRegInfo : SelfcheckSegsInfo) {
      std::vector<unsigned> RegIdxs(SelfcheckRegInfo.NumRegs);
      std::iota(RegIdxs.begin(), RegIdxs.end(),
                SelfcheckRegInfo.BaseDestRegister.id());
      std::copy(RegIdxs.begin(), RegIdxs.end(), std::back_inserter(Regs));
    }
    return Regs;
  }

  constexpr static auto UnlimitedPolicy = std::numeric_limits<unsigned>::max();

  GenPolicy
  getGenerationPolicy(const MachineBasicBlock &MBB, GeneratorContext &GenCtx,
                      std::optional<unsigned> BurstGroupID) const override {
    const auto &RISCVCtx =
        GenCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    auto Filter = getDefaultPolicyFilter(MBB, RISCVCtx);
    auto Overrides = RISCVCtx.getVSETOverrides(MBB);

    bool MustHavePrimaryInstr = RISCVCtx.hasActiveRVVMode(MBB);
    // Note: Filter can remove all opcodes
    return GenCtx.createGenerationPolicy(UnlimitedPolicy, std::move(Filter),
                                         MustHavePrimaryInstr, BurstGroupID,
                                         Overrides.getEntries());
  }

  void checkInstrTargetDependency(const OpcodeHistogram &H) const override {
    if (!checkSupportedOrdering(H))
      report_fatal_error("Lr.rl and Sc.aq are prohibited by RISCV ISA", false);
  }

  void generateRegsInit(MachineBasicBlock &MBB, const IRegisterState &R,
                        GeneratorContext &GC) const override {
    const auto &Regs = static_cast<const RISCVRegisterState &>(R);

    // Done before GPR initialization since scratch registers are used
    if (!Regs.FRegs.empty())
      generateFPRInit(MBB, Regs, GC);

    // Done before GPR initialization since scratch registers are used
    if (!Regs.VRegs.empty() && !UseSplatsForRVVInit.getValue())
      generateVRegsInit(MBB, Regs, GC);

    generateGPRInit(MBB, Regs, GC);

    if (!Regs.VRegs.empty() && UseSplatsForRVVInit.getValue())
      generateVRegsInitWithSplats(MBB, Regs, GC);
  }

  void generateGPRInit(MachineBasicBlock &MBB, const RISCVRegisterState &Regs,
                       GeneratorContext &GC) const {
    auto RP = GC.getRegisterPool();
    // Initialize registers (except X0) before taking a branch
    assert(Regs.XRegs[0] == 0);
    auto InsertPos = MBB.getFirstTerminator();
    for (auto [RegIdx, Value] : drop_begin(enumerate(Regs.XRegs))) {
      auto Reg = regIndexToMCReg(RegIdx, RegStorageType::XReg, GC);
      if (!RP.isReserved(Reg, MBB))
        writeValueToReg(MBB, InsertPos, APInt(getRegBitWidth(Reg, GC), Value),
                        Reg, RP, GC);
    }
  }

  void generateFPRInit(MachineBasicBlock &MBB, const RISCVRegisterState &Regs,
                       GeneratorContext &GC) const {
    auto RP = GC.getRegisterPool();
    // Initialize registers before taking a branch
    auto InsertPos = MBB.getFirstTerminator();
    for (auto [RegIdx, Value] : enumerate(Regs.FRegs)) {
      auto FPReg = regIndexToMCReg(RegIdx, RegStorageType::FReg, GC);
      if (!RP.isReserved(FPReg, MBB))
        writeValueToReg(MBB, InsertPos, APInt(getRegBitWidth(FPReg, GC), Value),
                        FPReg, RP, GC);
    }
  }

  void generateVRegsInit(MachineBasicBlock &MBB, const RISCVRegisterState &Regs,
                         GeneratorContext &GC) const {
    const auto &State = GC.getLLVMState();
    auto RP = GC.getRegisterPool();

    const auto &ST = GC.getSubtarget<RISCVSubtarget>();
    const auto &InstrInfo = State.getInstrInfo();

    assert(ST.hasStdExtV());

    // Initialize registers before taking a branch
    auto InsertPos = MBB.getFirstTerminator();

    // V0 to init before anything vector-related
    if (!NoMaskModeForRVV) {
      auto InitV0 = Regs.VRegs[0];
      writeValueToReg(MBB, InsertPos, InitV0, RISCV::V0, RP, GC);
    }
    rvvGenerateModeSwitchAndUpdateContext(InstrInfo, MBB, GC, InsertPos);

    // V0 is the mask register, skip it
    for (auto [RegIdx, Value] : drop_begin(enumerate(Regs.VRegs))) {
      auto Reg = regIndexToMCReg(RegIdx, RegStorageType::VReg, GC);
      // Skip reserved registers
      if (RP.isReserved(Reg, MBB))
        continue;
      writeValueToReg(MBB, InsertPos, Value, Reg, RP, GC);
    }
  }

  void generateVRegsInitWithSplats(MachineBasicBlock &MBB,
                                   const RISCVRegisterState &Regs,
                                   GeneratorContext &GC) const {
    const auto &State = GC.getLLVMState();
    auto RP = GC.getRegisterPool();

    const auto &ST = GC.getSubtarget<RISCVSubtarget>();
    const auto &InstrInfo = State.getInstrInfo();

    assert(ST.hasStdExtV());

    unsigned SEW = ST.getELEN();
    auto VLen = Regs.VLEN / SEW;
    bool TA = false;
    bool MA = false;

    auto InsertPos = MBB.getFirstTerminator();

    auto [RVVConfig, VLVM] =
        constructRVVModeWithVMReset(RISCVII::VLMUL::LMUL_1, VLen, SEW, TA, MA);
    generateRVVModeUpdate(InstrInfo, MBB, GC, RVVConfig, VLVM, InsertPos);

    // Initialize registers before taking a branch
    // V0 is the mask register, skip it.
    for (unsigned RegNo = 1; RegNo < Regs.XRegs.size(); ++RegNo) {
      auto XReg = regIndexToMCReg(RegNo, RegStorageType::XReg, GC);
      auto VReg = regIndexToMCReg(RegNo, RegStorageType::VReg, GC);
      if (!RP.isReserved(VReg, MBB))
        getSupportInstBuilder(MBB, InsertPos,
                              MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(RISCV::VMV_V_X), VReg)
            .addReg(XReg);
    }
  }

  bool is64Bit(const TargetMachine &TM) const override {
    assert(TM.getTargetTriple().isRISCV());
    return TM.getTargetTriple().isRISCV64();
  }

  bool isSelfcheckAllowed(unsigned Opcode) const override {
    if (isRVV(Opcode) && !SelfCheckRVV) {
      return false;
    }
    /*TODO: maybe need more conditions */
    return true;
  }

  bool isAtomicMemInstr(const MCInstrDesc &InstrDesc) const override {
    return isAtomicAMO(InstrDesc.getOpcode()) ||
           isScInstr(InstrDesc.getOpcode()) || isLrInstr(InstrDesc.getOpcode());
  }

  bool isDivOpcode(unsigned Opcode) const override {
    return Opcode == RISCV::DIV;
  }

  bool requiresCustomGeneration(const MCInstrDesc &InstrDesc) const override {
    auto Opcode = InstrDesc.getOpcode();
    if (isRVVModeSwitch(Opcode))
      return true;
    // NOTE: these checks are just a safety measure to control that we
    // process only supported instructions
    if (isLrInstr(Opcode) || isScInstr(Opcode))
      return false;
    if (isAtomicAMO(Opcode))
      return false;
    if (isSupportedLoadStore(Opcode))
      return false;
    // FIXME: all checks with fatal error should be moved to histogram verifier
    if (InstrDesc.mayLoad() || InstrDesc.mayStore())
      report_fatal_error("This memory instruction unsupported");

    // FIXME: here explicitly placed all vector istructions that use V0 mask
    // explicitly and this can not be changed
    if (NoMaskModeForRVV &&
        (isRVVuseV0RegExplicitly(Opcode) || isRVVuseV0RegImplicitly(Opcode))) {
      report_fatal_error("In histogram given a vector opcode with explicit V0 "
                         "mask usage, but snippy was given option that forbids "
                         "any masks for vector instructions",
                         false);
    }
    return false;
  }

  void getEncodedMCInstr(const MachineInstr *MI, const MCCodeEmitter &MCCE,
                         AsmPrinter &AP, const MCSubtargetInfo &STI,
                         SmallVector<char> &OutBuf) const override {
    MCInst OutInst;
    static_cast<RISCVAsmPrinter &>(AP).lowerToMCInst(MI, OutInst);

    SmallVector<MCFixup> Fixups;
    MCCE.encodeInstruction(OutInst, OutBuf, Fixups, STI);
  }

  void generateCustomInst(const MCInstrDesc &InstrDesc, MachineBasicBlock &MBB,
                          GeneratorContext &GC,
                          MachineBasicBlock::iterator Ins) const override {
    assert(requiresCustomGeneration(InstrDesc));
    auto Opcode = InstrDesc.getOpcode();
    assert(isRVVModeSwitch(Opcode));
    rvvGenerateModeSwitchAndUpdateContext(GC.getLLVMState().getInstrInfo(), MBB,
                                          GC, Ins, Opcode);
  }

  void instructionPostProcess(MachineInstr &MI, GeneratorContext &GC,
                              MachineBasicBlock::iterator Ins) const override;
  // From RISC-V spec v2.2:
  //     All branch instructions use the B-type instruction format. The 12-bit
  //     B-immediate encodes signed offsets in multiples of 2, and is added to
  //     the current pc to give the target address.
  static constexpr unsigned kMaxBranchDst = 1 << 13;
  // Max branch destination modulo
  static constexpr unsigned kMaxBranchDstMod = (kMaxBranchDst - 2) / 2;
  // Same for compressed branches
  static constexpr unsigned kMaxCompBranchDst = 1 << 9;
  // Max branch destination modulo
  static constexpr unsigned kMaxCompBranchDstMod = (kMaxCompBranchDst - 2) / 2;
  // Same for unconditional branches
  static constexpr unsigned kMaxJumpDst = 1 << 22;
  // Max branch destination modulo
  static constexpr unsigned kMaxJumpDstMod = (kMaxJumpDst - 2) / 2;

  static constexpr unsigned kMaxInstrSize = 4;

  unsigned getMaxInstrSize() const override { return kMaxInstrSize; }

  unsigned getMaxBranchDstMod(unsigned Opcode) const override {
    return isCompressedBranch(Opcode) ? kMaxCompBranchDstMod : kMaxBranchDstMod;
  }

  MachineBasicBlock *
  getBranchDestination(const MachineInstr &Branch) const override {
    assert(Branch.isBranch() && "Only branches expected");
    auto DestBBOpNum = Branch.getNumExplicitOperands() - 1;
    return Branch.getOperand(DestBBOpNum).getMBB();
  }

  bool branchNeedsVerification(const MachineInstr &Branch) const override {
    assert(Branch.isBranch());

    switch (Branch.getOpcode()) {
    case RISCV::C_BEQZ:
    case RISCV::C_BNEZ:
    case RISCV::BEQ:
    case RISCV::BNE:
    case RISCV::BGE:
    case RISCV::BLT:
    case RISCV::BGEU:
    case RISCV::BLTU:
      return true;
    default:
      return false;
    }
  }

  MachineBasicBlock *generateBranch(const MCInstrDesc &InstrDesc,
                                    MachineBasicBlock &MBB,
                                    GeneratorContext &GC) const override {
    auto &State = GC.getLLVMState();
    auto RP = GC.getRegisterPool();
    auto *MF = MBB.getParent();
    auto *NextMBB = MF->CreateMachineBasicBlock();
    MF->insert(++MachineFunction::iterator(&MBB), NextMBB);
    NextMBB->transferSuccessorsAndUpdatePHIs(&MBB);
    MBB.addSuccessor(NextMBB);

    auto Opcode = InstrDesc.getOpcode();
    const auto &InstrInfo = State.getInstrInfo();
    const auto &BranchDesc = InstrInfo.get(Opcode);
    if (BranchDesc.isUnconditionalBranch()) {
      const auto *RVInstrInfo =
          GC.getSubtarget<RISCVSubtarget>().getInstrInfo();
      RVInstrInfo->insertUnconditionalBranch(MBB, NextMBB, DebugLoc());
      return NextMBB;
    }

    const auto &RegInfo = State.getRegInfo();
    auto MIB = BuildMI(MBB, MBB.end(), MIMetadata(), BranchDesc);
    const auto &MCRegClass =
        RegInfo.getRegClass(BranchDesc.operands()[0].RegClass);
    auto FirstReg = RP.getAvailableRegister("for branch condition", RegInfo,
                                            MCRegClass, MBB);
    MIB.addReg(FirstReg);
    if (!isCompressedBranch(Opcode)) {
      auto SecondReg = RP.getAvailableRegister("for branch condition", RegInfo,
                                               MCRegClass, MBB);
      MIB.addReg(SecondReg);
    }
    MIB.addMBB(NextMBB);

    insertFallbackBranch(MBB, *NextMBB, State);
    return NextMBB;
  }

  bool fitsCompressedBranch(unsigned Distance) const {
    return Distance <= kMaxCompBranchDstMod;
  }

  bool fitsBranch(unsigned Distance) const {
    return Distance <= kMaxBranchDstMod;
  }

  bool fitsJump(unsigned Distance) const { return Distance <= kMaxJumpDstMod; }

  MachineInstr *relaxCompressedBranch(MachineInstr &Branch,
                                      GeneratorContext &GC) const {
    auto Opcode = Branch.getOpcode();
    assert(isCompressedBranch(Opcode) && "Compressed branch expected");
    auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto UncompOpcode = Opcode == RISCV::C_BEQZ ? RISCV::BEQ : RISCV::BNE;
    auto *MBB = Branch.getParent();
    assert(MBB);
    auto CondOp = Branch.getOperand(0);
    assert(CondOp.isReg());
    auto CondReg = CondOp.getReg();
    auto *DstMBB = getBranchDestination(Branch);
    assert(DstMBB);
    auto &MI = *BuildMI(*MBB, Branch, MIMetadata(), InstrInfo.get(UncompOpcode))
                    .addReg(CondReg)
                    .addReg(RISCV::X0)
                    .addMBB(DstMBB);
    Branch.eraseFromParent();
    return &MI;
  }

  // Replacing branch with opposite branch to fallback BB and setting fallback
  // branch to target BB. Example:
  //     ...                                ...
  //     BEQ $x1, $x0, %bb.1000     =>      BNE $x1, $x0, %bb.1
  //     PseudoBR %bb.1                     PseudoBR %bb.1000
  //     ...                                ...
  MachineInstr *relaxWithJump(MachineInstr &Branch,
                              GeneratorContext &GC) const {
    auto *ProcessedBranch = &Branch;
    auto *MBB = Branch.getParent();
    assert(MBB);

    // We need to uncompress branch because all actions below don't expect
    // compressed branch
    if (isCompressedBranch(Branch.getOpcode()))
      ProcessedBranch = relaxCompressedBranch(Branch, GC);
    assert(ProcessedBranch);

    const auto *InstrInfo = MBB->getParent()->getSubtarget().getInstrInfo();
    assert(InstrInfo);
    const auto &RVInstrInfo = static_cast<const RISCVInstrInfo &>(*InstrInfo);
    MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
    SmallVector<MachineOperand> Cond;
    assert(MBB);
    RVInstrInfo.analyzeBranch(*MBB, TBB, FBB, Cond,
                              /* AllowModify */ false);
    assert(TBB && FBB);

    RVInstrInfo.reverseBranchCondition(Cond);
    auto *FallbackBR = ProcessedBranch->getNextNode();
    assert(FallbackBR && "Fallback branch expected");
    assert(*FallbackBR != MBB->end() && "Fallback branch expected");
    assert(checkSupportMetadata(*FallbackBR));
    ProcessedBranch->eraseFromParent();
    FallbackBR->eraseFromParent();
    RVInstrInfo.insertBranch(*MBB, FBB, TBB, Cond, DebugLoc());
    setAsSupportInstr(MBB->back(), GC.getLLVMState().getCtx());
    return &*MBB->getFirstTerminator();
  }

  bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                   GeneratorContext &GC) const override {
    if (fitsCompressedBranch(Distance))
      return true;
    if (fitsBranch(Distance) && isCompressedBranch(Branch.getOpcode()))
      return relaxCompressedBranch(Branch, GC) != nullptr;
    if (fitsJump(Distance))
      return relaxWithJump(Branch, GC) != nullptr;

    return false;
  }

  void insertFallbackBranch(MachineBasicBlock &From, MachineBasicBlock &To,
                            const LLVMState &State) const override {
    const auto &InstrInfo = State.getInstrInfo();
    getSupportInstBuilder(From, From.end(),
                          From.getParent()->getFunction().getContext(),
                          InstrInfo.get(RISCV::PseudoBR))
        .addMBB(&To);
  }

  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock &NewDestMBB) const override {
    auto *OldDestBB = getBranchDestination(Branch);
    if (OldDestBB == &NewDestMBB)
      return false;

    auto DestBBOpNum = Branch.getNumExplicitOperands() - 1;
    Branch.removeOperand(DestBBOpNum);

    auto NewDestOperand = MachineOperand::CreateMBB(&NewDestMBB);
    Branch.addOperand(NewDestOperand);

    auto *BranchBB = Branch.getParent();

    if (Branch.isConditionalBranch()) {
      auto &FallbackBranch = *Branch.getNextNode();
      assert(FallbackBranch.isBranch());
      auto *FallbackDestMBB = getBranchDestination(FallbackBranch);
      if (FallbackDestMBB != OldDestBB)
        BranchBB->replaceSuccessor(OldDestBB, &NewDestMBB);
      else
        BranchBB->addSuccessor(&NewDestMBB);
    } else {
      BranchBB->replaceSuccessor(OldDestBB, &NewDestMBB);
    }

    return true;
  }

  bool replaceBranchDest(MachineBasicBlock &BranchMBB,
                         MachineBasicBlock &OldDestMBB,
                         MachineBasicBlock &NewDestMBB) const override {
    if (&OldDestMBB == &NewDestMBB)
      return false;

    auto NewDestOperand = MachineOperand::CreateMBB(&NewDestMBB);
    for (auto &Branch : BranchMBB.terminators()) {
      auto *BranchDestMBB = getBranchDestination(Branch);
      if (BranchDestMBB != &OldDestMBB)
        continue;
      auto DestBBOpNum = Branch.getNumExplicitOperands() - 1;
      Branch.removeOperand(DestBBOpNum);
      Branch.addOperand(NewDestOperand);
    }

    BranchMBB.replaceSuccessor(&OldDestMBB, &NewDestMBB);

    return true;
  }

  SmallVector<unsigned>
  getImmutableRegs(const MCRegisterClass &MCRegClass) const override {
    SmallVector<unsigned> ImmutableRegsNums;
    if (MCRegClass.contains(RISCV::X0))
      return {RISCV::X0};
    return {};
  }

  /// RISCV Loops:
  ///
  /// * BEQ, C_BEQZ:
  ///   -- Init --
  ///     CounterReg = 0
  ///   -- Latch --
  ///     CounterReg += 1
  ///     LimitReg = CounterReg >> log2(NIter)
  ///   -- Branch --
  ///     BEQ LimitReg, X0
  ///     C_BEQZ LimitReg
  ///
  /// * BNE, C_BNEZ:
  ///   -- Init --
  ///     LimitReg = 0
  ///     CounterReg = NIter
  ///   -- Latch --
  ///     CounterReg -= 1
  ///   -- Branch --
  ///     BNE CounterReg, LimitReg
  ///     C_BNEZ CounterReg
  ///
  /// * BLT, BLTU:
  ///   -- Init --
  ///     LimitReg = NIter
  ///     CounterReg = 0
  ///   -- Latch --
  ///     CounterReg += 1
  ///   -- Branch --
  ///     BLT CounterReg, LimitReg
  ///     BLTU CounterReg, LimitReg
  ///
  /// * BGE, BGEU:
  ///   -- Init --
  ///     LimitReg = 0
  ///     CounterReg = NIter
  ///   -- Latch --
  ///     CounterReg -= 1
  ///   -- Branch --
  ///     BGE CounterReg, LimitReg
  ///     BGEU CounterReg, LimitReg

  MachineInstr &updateLoopBranch(MachineInstr &Branch,
                                 const MCInstrDesc &InstrDesc,
                                 Register CounterReg,
                                 Register LimitReg) const override {
    assert(Branch.isBranch() && "Branch expected");
    auto *BranchMBB = Branch.getParent();
    auto Opcode = Branch.getOpcode();
    bool EqBranch = isEqBranch(Opcode);
    auto FirstReg = EqBranch ? LimitReg : CounterReg;
    auto *DestBB = getBranchDestination(Branch);
    auto NewBranch =
        BuildMI(*BranchMBB, Branch, MIMetadata(), InstrDesc).addReg(FirstReg);
    if (!isCompressedBranch(Opcode)) {
      auto SecondReg = EqBranch ? RISCV::X0 : LimitReg;
      NewBranch.addReg(SecondReg);
    }
    NewBranch.addMBB(DestBB);

    return *NewBranch.getInstr();
  }

  void insertLoopInit(MachineBasicBlock &MBB, MachineBasicBlock::iterator Pos,
                      MachineInstr &Branch, Register CounterReg,
                      Register LimitReg, unsigned NIter,
                      GeneratorContext &GC) const override {
    assert(Branch.isBranch() && "Branch expected");
    assert(CounterReg != LimitReg &&
           "Counter and limit registers expected to be different");
    auto RP = GC.getRegisterPool();

    switch (Branch.getOpcode()) {
    case RISCV::BEQ:
    case RISCV::C_BEQZ:
      writeValueToReg(MBB, Pos, APInt::getZero(getRegBitWidth(CounterReg, GC)),
                      CounterReg, RP, GC);
      // FIXME: LimitReg won't be used by the loop. We write value to it just to
      // make sure it is initialized.
      writeValueToReg(MBB, Pos, APInt::getZero(getRegBitWidth(LimitReg, GC)),
                      LimitReg, RP, GC);
      break;
    case RISCV::BNE:
      writeValueToReg(MBB, Pos, APInt(getRegBitWidth(CounterReg, GC), NIter),
                      CounterReg, RP, GC);
      writeValueToReg(MBB, Pos, APInt::getZero(getRegBitWidth(LimitReg, GC)),
                      LimitReg, RP, GC);
      break;
    case RISCV::C_BNEZ:
      writeValueToReg(MBB, Pos, APInt(getRegBitWidth(CounterReg, GC), NIter),
                      CounterReg, RP, GC);
      // FIXME: LimitReg won't be used by the loop. We write value to it just to
      // make sure it is initialized.
      writeValueToReg(MBB, Pos, APInt::getZero(getRegBitWidth(LimitReg, GC)),
                      LimitReg, RP, GC);
      break;
    case RISCV::BLT:
    case RISCV::BLTU:
      writeValueToReg(MBB, Pos, APInt::getZero(getRegBitWidth(CounterReg, GC)),
                      CounterReg, RP, GC);
      writeValueToReg(MBB, Pos, APInt(getRegBitWidth(LimitReg, GC), NIter),
                      LimitReg, RP, GC);
      break;
    case RISCV::BGE:
    case RISCV::BGEU:
      writeValueToReg(MBB, Pos, APInt(getRegBitWidth(CounterReg, GC), NIter),
                      CounterReg, RP, GC);
      writeValueToReg(MBB, Pos, APInt(getRegBitWidth(LimitReg, GC), 1),
                      LimitReg, RP, GC);
      break;
    default:
      llvm_unreachable("Unsupported branch type");
    }
  }

  LoopCounterInsertionResult
  insertLoopCounter(MachineBasicBlock::iterator Pos, MachineInstr &Branch,
                    Register CounterReg, Register LimitReg, unsigned NIter,
                    GeneratorContext &GC,
                    RegToValueType &ExitingValues) const override {
    assert(Branch.isBranch() && "Branch expected");
    assert(CounterReg != LimitReg &&
           "Counter and limit registers expected to be different");
    assert(NIter);

    auto &State = GC.getLLVMState();
    auto &MBB = *Pos->getParent();
    const auto &InstrInfo = State.getInstrInfo();
    APInt MinCounterVal;

    switch (Branch.getOpcode()) {
    case RISCV::BEQ:
    case RISCV::C_BEQZ:
      // Closest power of two (floor)
      NIter = bit_floor(NIter);
      getSupportInstBuilder(MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::ADDI))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(1);
      getSupportInstBuilder(MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::SRLI))
          .addReg(LimitReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(Log2_32(NIter));
      ExitingValues[CounterReg] =
          APInt(getRegBitWidth(CounterReg, GC), bit_floor(NIter));
      MinCounterVal = APInt(getRegBitWidth(LimitReg, GC), 1);
      ExitingValues[LimitReg] = APInt(getRegBitWidth(LimitReg, GC), 1);

      if (!isPowerOf2_32(NIter))
        return {SnippyDiagnosticInfo(
                    "Number of iterations is not power of 2",
                    "Number of iterations for BEQ and C_BEQZ "
                    "will be reduced to the nearest power of 2",
                    llvm::DS_Warning, WarningName::LoopIterationNumber),
                NIter, MinCounterVal};
      break;
    case RISCV::BNE:
    case RISCV::C_BNEZ:
      getSupportInstBuilder(MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::ADDI))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(-1);
      ExitingValues[CounterReg] = APInt(getRegBitWidth(CounterReg, GC), 0);
      MinCounterVal = APInt(getRegBitWidth(CounterReg, GC), 0);
      break;
    case RISCV::BLT:
    case RISCV::BLTU:
      getSupportInstBuilder(MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::ADDI))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(1);
      ExitingValues[CounterReg] = APInt(getRegBitWidth(CounterReg, GC), NIter);
      MinCounterVal = APInt(getRegBitWidth(CounterReg, GC), 1);
      break;
    case RISCV::BGE:
    case RISCV::BGEU:
      getSupportInstBuilder(MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::ADDI))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(-1);
      ExitingValues[CounterReg] = APInt(getRegBitWidth(CounterReg, GC), 0);
      MinCounterVal = APInt(getRegBitWidth(CounterReg, GC), 0);
      break;
    default:
      llvm_unreachable("Unsupported branch type");
    }

    return {std::nullopt, NIter, MinCounterVal};
  }

  // * 1 or 2 instructions for init
  // * Fallback branch for preheader
  // * 1 or 2 instructions for latch
  static constexpr auto kOverheadPerLoop = 4;

  unsigned getLoopOverhead() const override { return kOverheadPerLoop; }

  MachineInstr *generateFinalInst(MachineBasicBlock &MBB, GeneratorContext &GC,
                                  unsigned LastInstrOpc) const override {
    const auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto MIB = getSupportInstBuilder(
        MBB, MBB.end(), MBB.getParent()->getFunction().getContext(),
        InstrInfo.get(LastInstrOpc));
    return MIB;
  }

  std::optional<unsigned>
  findRegisterByName(const StringRef RegName) const override {
    Register Reg = 0;
    Reg = MatchRegisterAltName(RegName);
    if (Reg == RISCV::NoRegister)
      Reg = MatchRegisterName(RegName);
    if (Reg == RISCV::NoRegister)
      return std::nullopt;
    else
      return Reg;
  }

  unsigned getSpillAlignmentInBytes(MCRegister Reg,
                                    GeneratorContext &GC) const override {
    // TODO: return actual minimum alignment of Reg.
    return 16u;
  }

  unsigned getRegBitWidth(MCRegister Reg, GeneratorContext &GC) const override {
    const auto &ST = GC.getSubtarget<RISCVSubtarget>();
    auto VLEN =
        GC.getTargetContext().getImpl<RISCVGeneratorContext>().getVLEN();
    return snippy::getRegBitWidth(Reg, ST.getXLen(), VLEN);
  }

  MCRegister regIndexToMCReg(unsigned RegIdx, RegStorageType Storage,
                             GeneratorContext &GC) const override {
    const auto &ST = GC.getSubtarget<RISCVSubtarget>();
    return snippy::regIndexToMCReg(RegIdx, Storage, ST.hasStdExtD());
  }

  RegStorageType regToStorage(Register Reg) const override {
    return snippy::regToStorage(Reg);
  }

  unsigned regToIndex(Register Reg) const override {
    return snippy::regToIndex(Reg);
  }

  unsigned getNumRegs(RegStorageType Storage,
                      const TargetSubtargetInfo &SubTgt) const override {
    const auto &ST = static_cast<const RISCVSubtarget &>(SubTgt);
    switch (Storage) {
    case RegStorageType::XReg:
      return 32u;
    case RegStorageType::FReg:
      return (ST.hasStdExtF() || ST.hasStdExtD()) ? 32u : 0u;
    case RegStorageType::VReg:
      return ST.hasStdExtV() ? 32u : 0u;
    }
    llvm_unreachable("Unknown storage type");
  }

  unsigned getSpillSizeInBytes(MCRegister Reg,
                               GeneratorContext &GC) const override {
    unsigned RegSize = getRegBitWidth(Reg, GC) / RISCV_CHAR_BIT;
    auto Alignment = getSpillAlignmentInBytes(Reg, GC);
    assert(Alignment && "Alignment size can't be zero");
    // Get the least number of alignment sizes that fully fits register.
    return Alignment * divideCeil(RegSize, Alignment);
  }

  std::vector<MCRegister> getRegsPreservedByABI() const override {
    return {RISCV::X1 /* Return address */, RISCV::X3 /* Global pointer */,
            RISCV::X4 /* Thread pointer */, RISCV::X8 /* Frame pointer */,
            /* Saved registers (s1-s11) */
            RISCV::X9, RISCV::X18, RISCV::X19, RISCV::X20, RISCV::X21,
            RISCV::X22, RISCV::X23, RISCV::X24, RISCV::X25, RISCV::X26,
            RISCV::X27};
  }

  MCRegister getStackPointer() const override { return RISCV::X2; }

  void generateSpill(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                     MCRegister Reg, GeneratorContext &GC) const override {
    assert(GC.stackEnabled() &&
           "An attempt to generate spill but stack was not enabled.");
    auto SP = getStackPointer();

    auto &State = GC.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();
    getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(-static_cast<int64_t>(getSpillSizeInBytes(Reg, GC)));

    storeRegToAddrInReg(MBB, Ins, SP, Reg, GC);
  }

  void generateReload(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                      MCRegister Reg, GeneratorContext &GC) const override {
    assert(GC.stackEnabled() &&
           "An attempt to generate reload but stack was not enabled.");
    auto SP = getStackPointer();

    auto &State = GC.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();

    loadRegFromAddrInReg(MBB, Ins, SP, Reg, GC);
    getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(getSpillSizeInBytes(Reg, GC));
  }

  void generatePopNoReload(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator Ins, MCRegister Reg,
                           GeneratorContext &GC) const override {
    assert(GC.stackEnabled() &&
           "An attempt to generate stack pop but stack was not enabled.");
    auto SP = getStackPointer();

    auto &State = GC.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();

    getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(getSpillSizeInBytes(Reg, GC));
  }

  MachineInstr *generateCall(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins,
                             const Function &Target, GeneratorContext &GC,
                             bool AsSupport) const override {
    return generateCall(MBB, Ins, Target, GC, AsSupport, RISCV::JAL);
  }

  MachineInstr *loadSymbolAddress(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator Ins,
                                  GeneratorContext &GC, unsigned DestReg,
                                  const GlobalValue *Target) const {
    const auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto &State = GC.getLLVMState();
    auto &Ctx = State.getCtx();
    MachineFunction *MF = MBB.getParent();

    // Cannot emit PseudoLLA here, because this pseudo instruction is expanded
    // by RISCVPreRAExpandPseudo pass, which runs before register allocation.
    // That's why create auipc + addi pair manually.

    MachineInstr *MIAUIPC =
        getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::AUIPC))
            .addReg(DestReg)
            .addGlobalAddress(Target, 0, RISCVII::MO_PCREL_HI);
    MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("pcrel_hi");
    MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

    return getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addReg(DestReg)
        .addReg(DestReg)
        .addSym(AUIPCSymbol, RISCVII::MO_PCREL_LO);
  }

  MachineInstr *generateJAL(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Ins,
                            const Function &Target, GeneratorContext &GC,
                            bool AsSupport) const {
    const auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto &State = GC.getLLVMState();
    auto &Ctx = State.getCtx();
    // Despite PseudoCALL gets expanded by RISCVMCCodeEmitter to JALR
    // instruction, it has chance to be relaxed back to JAL by linker.
    return getInstBuilder(AsSupport, MBB, Ins, Ctx,
                          InstrInfo.get(RISCV::PseudoCALL))
        .addGlobalAddress(&Target, 0, RISCVII::MO_CALL);
  }

  MachineInstr *generateJALR(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins,
                             const Function &Target, GeneratorContext &GC,
                             bool AsSupport) const {
    const auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto &State = GC.getLLVMState();
    auto &Ctx = State.getCtx();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    auto RP = GC.getRegisterPool();
    auto Reg = getNonZeroReg("scratch register for storing function address",
                             RI, RegClass, RP, MBB);
    loadSymbolAddress(MBB, Ins, GC, Reg, &Target);
    return getInstBuilder(AsSupport, MBB, Ins, Ctx,
                          InstrInfo.get(RISCV::PseudoCALLIndirect))
        .addReg(Reg);
  }

  MachineInstr *generateCall(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Ins,
                             const Function &Target, GeneratorContext &GC,
                             bool AsSupport,
                             unsigned PreferredCallOpcode) const override {
    assert(isCall(PreferredCallOpcode) && "Expected call here");
    switch (PreferredCallOpcode) {
    case RISCV::JAL:
      return generateJAL(MBB, Ins, Target, GC, AsSupport);
    case RISCV::JALR:
      return generateJALR(MBB, Ins, Target, GC, AsSupport);
    default:
      report_fatal_error("Unsupported call instruction", false);
    }
  }

  MachineInstr *generateTailCall(MachineBasicBlock &MBB, const Function &Target,
                                 const GeneratorContext &GC) const override {
    const auto &InstrInfo = GC.getLLVMState().getInstrInfo();
    auto &State = GC.getLLVMState();
    auto &Ctx = State.getCtx();
    return getSupportInstBuilder(MBB, MBB.end(), Ctx,
                                 InstrInfo.get(RISCV::PseudoTAIL))
        .addGlobalAddress(&Target, 0, RISCVII::MO_CALL);
  }

  MachineInstr *generateReturn(MachineBasicBlock &MBB,
                               const LLVMState &State) const override {
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB = getSupportInstBuilder(
        MBB, MBB.end(), MBB.getParent()->getFunction().getContext(),
        InstrInfo.get(RISCV::PseudoRET));
    return MIB;
  }

  MachineInstr *generateNop(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Ins,
                            const LLVMState &State) const override {
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB = getSupportInstBuilder(
                   MBB, Ins, MBB.getParent()->getFunction().getContext(),
                   InstrInfo.get(RISCV::ADDI), RISCV::X0)
                   .addReg(RISCV::X0)
                   .addImm(0);
    return MIB;
  }

  unsigned getTransformSequenceLength(APInt OldValue, APInt NewValue,
                                      MCRegister Register,
                                      GeneratorContext &GC) const override {
    if (!RISCV::GPRRegClass.contains(Register))
      report_fatal_error(
          "transform of value in register is supported only for GPR", false);

    if (NewValue.eq(OldValue))
      return 0u;

    bool WillUseAdd = NewValue.ugt(OldValue);
    bool Overflowed = false;
    APInt ValueToWrite =
        APInt(WillUseAdd ? NewValue.usub_ov(OldValue, Overflowed)
                         : OldValue.usub_ov(NewValue, Overflowed));
    assert(!Overflowed && "Expression expect to not overflow");

    // Transform sequence has length of materialization of ValueToWwite in
    // scratch register plus one operation
    // (that is Register = ADD/SUB Register, ScratchReg).
    return getWriteValueSequenceLength(ValueToWrite, Register, GC) + 1;
  }

  void transformValueInReg(MachineBasicBlock &MBB,
                           const MachineBasicBlock::iterator &Ins,
                           APInt OldValue, APInt NewValue, MCRegister Register,
                           RegPoolWrapper &RP,
                           GeneratorContext &GC) const override {
    if (!RISCV::GPRRegClass.contains(Register))
      report_fatal_error(
          "transform of value in register is supported only for GPR", false);

    auto &State = GC.getLLVMState();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    const auto &InstrInfo = State.getInstrInfo();

    if (NewValue.eq(OldValue))
      return;

    // Materialization sequence should not touch Register.
    RP.addReserved(Register, AccessMaskBit::W);

    if (!hasNonZeroRegAvailable(RegClass, RP, AccessMaskBit::W)) {
      writeValueToReg(MBB, Ins, NewValue, Register, RP, GC);
      return;
    }

    // First need to choose another not X0 reg to materialize
    // differnce value in.
    auto ScratchReg = getNonZeroReg("scratch register for transforming value",
                                    RI, RegClass, RP, MBB);

    // Choose final operation based on value relation.
    bool WillUseAdd = NewValue.ugt(OldValue);
    bool Overflowed = false;
    auto ValueToWrite =
        APInt(WillUseAdd ? NewValue.usub_ov(OldValue, Overflowed)
                         : OldValue.usub_ov(NewValue, Overflowed));
    writeValueToReg(MBB, Ins, ValueToWrite, ScratchReg, RP, GC);
    assert(!Overflowed && "Expression expect to not overflow");
    assert(
        getTransformSequenceLength(OldValue, NewValue, Register, GC) ==
            (getWriteValueSequenceLength(ValueToWrite, ScratchReg, GC) + 1) &&
        "Generated sequence length does not match expected one");
    getSupportInstBuilder(MBB, Ins, State.getCtx(),
                          InstrInfo.get(WillUseAdd ? RISCV::ADD : RISCV::SUB),
                          Register)
        .addReg(Register)
        .addReg(ScratchReg);
  }

  void loadEffectiveAddressInReg(MachineBasicBlock &MBB,
                                 const MachineBasicBlock::iterator &Ins,
                                 MCRegister Register, uint64_t BaseAddr,
                                 uint64_t Stride, MCRegister IndexReg,
                                 RegPoolWrapper &RP,
                                 GeneratorContext &GC) const override {
    assert(RISCV::GPRRegClass.contains(Register, IndexReg) &&
           "Only GPR registers are supported");

    auto &State = GC.getLLVMState();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    const auto &InstrInfo = State.getInstrInfo();

    auto XRegBitSize = getRegBitWidth(Register, GC);
    writeValueToReg(MBB, Ins, APInt(XRegBitSize, Stride), Register, RP, GC);
    getSupportInstBuilder(MBB, Ins, State.getCtx(), InstrInfo.get(RISCV::MUL),
                          Register)
        .addReg(Register)
        .addReg(IndexReg);

    RP.addReserved(Register);
    if (!hasNonZeroRegAvailable(RegClass, RP))
      report_fatal_error("Can't find suitable scratch register");

    auto AddrReg =
        getNonZeroReg("Scratch register for BaseAddr", RI, RegClass, RP, MBB);

    writeValueToReg(MBB, Ins, APInt(XRegBitSize, BaseAddr), AddrReg, RP, GC);
    getSupportInstBuilder(MBB, Ins, State.getCtx(), InstrInfo.get(RISCV::ADD),
                          Register)
        .addReg(Register)
        .addReg(AddrReg);
  }

  MachineOperand createOperandForOpType(const ImmediateHistogramSequence *IH,
                                        unsigned OperandType,
                                        const StridedImmediate &StridedImm,
                                        const TargetMachine &TM) const {
    // NOTE: need to be in sync with
    // llvm/lib/Target/RISCV/MCTargetDesc/RISCVBaseInfo.h(RISCVOp)
    // llvm/lib/Target/RISCV/RISCVInstrInfo.cpp
    // (RISCVInstrInfo::verifyInstruction)
    switch (OperandType) {
    default:
      report_fatal_error(
          "Requested generation for an unexpected operand type.");
    case RISCVOp::OPERAND_UIMM1:
      return MachineOperand::CreateImm(genImmUINT<1>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM2:
      return MachineOperand::CreateImm(genImmUINT<2>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM2_LSB0:
      return MachineOperand::CreateImm(
          genImmUINTWithNZeroLSBs<2, 1>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM3:
      return MachineOperand::CreateImm(genImmUINT<3>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM4:
      return MachineOperand::CreateImm(genImmUINT<4>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM5:
      return MachineOperand::CreateImm(genImmUINT<5>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM5_NONZERO:
      return MachineOperand::CreateImm(genImmNonZeroUINT<5>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM6:
      return MachineOperand::CreateImm(genImmUINT<6>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM7:
      return MachineOperand::CreateImm(genImmUINT<7>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM7_LSB00:
      return MachineOperand::CreateImm(
          genImmUINTWithNZeroLSBs<7, 2>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM8:
      return MachineOperand::CreateImm(genImmUINT<8>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM8_LSB00:
      return MachineOperand::CreateImm(
          genImmUINTWithNZeroLSBs<8, 2>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM8_GE32:
      return MachineOperand::CreateImm(
          genImmInInterval<32, 1 << 8>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM8_LSB000:
      return MachineOperand::CreateImm(
          genImmUINTWithNZeroLSBs<8, 3>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM9_LSB000:
      return MachineOperand::CreateImm(
          genImmUINTWithNZeroLSBs<9, 3>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM12:
      return MachineOperand::CreateImm(genImmUINT<12>(IH, StridedImm));
    case RISCVOp::OPERAND_ZERO:
      return MachineOperand::CreateImm(0);
    case RISCVOp::OPERAND_SIMM5:
      return MachineOperand::CreateImm(genImmSINT<5>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM5_PLUS1:
      return MachineOperand::CreateImm(
          genImmSINTWithOffset<5, 1>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM6:
      return MachineOperand::CreateImm(genImmSINT<6>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM6_NONZERO:
      return MachineOperand::CreateImm(genImmNonZeroSINT<6>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM10_LSB0000_NONZERO:
      return MachineOperand::CreateImm(
          genImmNonZeroSINTWithNZeroLSBs<10, 4>(IH, StridedImm));
    case RISCVOp::OPERAND_CLUI_IMM:
      return MachineOperand::CreateImm(
          genImmSINTWithOffset<5, 0xffff0>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM10_LSB00_NONZERO:
      return MachineOperand::CreateImm(
          genImmNonZeroUINTWithNZeroLSBs<10, 2>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM12:
      return MachineOperand::CreateImm(genImmSINT<12>(IH, StridedImm));
    case RISCVOp::OPERAND_SIMM12_LSB00000:
      return MachineOperand::CreateImm(
          genImmSINTWithNZeroLSBs<12, 5>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMM20:
      return MachineOperand::CreateImm(genImmUINT<20>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMMLOG2XLEN:
      if (is64Bit(TM))
        return MachineOperand::CreateImm(genImmUINT<6>(IH, StridedImm));
      return MachineOperand::CreateImm(genImmUINT<5>(IH, StridedImm));
    case RISCVOp::OPERAND_UIMMLOG2XLEN_NONZERO:
      if (is64Bit(TM))
        return MachineOperand::CreateImm(genImmNonZeroUINT<6>(IH, StridedImm));
      return MachineOperand::CreateImm(genImmNonZeroUINT<5>(IH, StridedImm));
    case RISCVOp::OPERAND_VTYPEI10:
    case RISCVOp::OPERAND_VTYPEI11:
      assert(false && "VTYPE immediates should not be randomly sampled");
    case RISCVOp::OPERAND_RVKRNUM:
      return MachineOperand::CreateImm(genImmInInterval<0, 10>(IH, StridedImm));
    case RISCVOp::OPERAND_RVKRNUM_0_7:
      return MachineOperand::CreateImm(genImmInInterval<0, 7>(IH, StridedImm));
    case RISCVOp::OPERAND_RVKRNUM_1_10:
      return MachineOperand::CreateImm(genImmInInterval<1, 10>(IH, StridedImm));
    case RISCVOp::OPERAND_RVKRNUM_2_14:
      return MachineOperand::CreateImm(genImmInInterval<2, 14>(IH, StridedImm));
    case RISCVOp::OPERAND_AVL:
      report_fatal_error("AVL operand generation is not supported. Probably "
                         "snippy still does "
                         "not support vector instructions generation.");
    case RISCVOp::OPERAND_FRMARG:
      return MachineOperand::CreateImm(0);
    }
  }

  MachineOperand genTargetOpForOpcode(unsigned Opcode, unsigned OperandType,
                                      const StridedImmediate &StridedImm,
                                      GeneratorContext &GC) const {
    const auto &TM = GC.getLLVMState().getTargetMachine();
    const auto &OpcSetting = GC.getOpcodeToImmHistMap().getConfigForOpcode(
        Opcode, GC.getOpcodeCache());
    if (OpcSetting.isUniform())
      return createOperandForOpType(nullptr, OperandType, StridedImm, TM);
    const auto &Seq = OpcSetting.getSequence();
    return createOperandForOpType(&Seq, OperandType, StridedImm, TM);
  }

  MachineOperand
  generateTargetOperand(GeneratorContext &SGCtx, unsigned Opcode,
                        unsigned OperandType,
                        const StridedImmediate &StridedImm) const override {
    auto *IHV = &SGCtx.getConfig().ImmHistogram;
    if (IHV && IHV->holdsAlternative<ImmediateHistogramRegEx>())
      return genTargetOpForOpcode(Opcode, OperandType, StridedImm, SGCtx);
    const ImmediateHistogramSequence *IH =
        IHV ? &IHV->get<ImmediateHistogramSequence>() : nullptr;
    if (isSupportedLoadStore(Opcode))
      // Disable histogram for loads and stores
      IH = nullptr;
    else if (IH->Values.empty())
      IH = nullptr;
    return createOperandForOpType(IH, OperandType, StridedImm,
                                  SGCtx.getLLVMState().getTargetMachine());
  }

  AccessMaskBit getCustomAccessMaskForOperand(GeneratorContext &SGCtx,
                                              const MCInstrDesc &InstrDesc,
                                              unsigned Operand) const override {
    auto Opcode = InstrDesc.getOpcode();
    if (!isRVVIndexedLoadStore(Opcode) && !isRVVIndexedSegLoadStore(Opcode) &&
        !isRVVStridedLoadStore(Opcode) && !isRVVStridedSegLoadStore(Opcode))
      return AccessMaskBit::None;

    // Both indexed and stride rvv load/stores has additional 'memory operand'
    // right after main address operand.
    auto MemOpIdx = getMemOperandIdx(InstrDesc);
    assert((InstrDesc.getNumOperands() > MemOpIdx + 1) &&
           "Expected index/stride operand");
    assert(std::next(InstrDesc.operands().begin(), MemOpIdx + 1)->OperandType ==
           MCOI::OPERAND_REGISTER);

    // That operand will be later referenced in breakDownAddr() and should
    // be available for writing despite being source operand for instruction.
    if (MemOpIdx + 1 == Operand)
      return AccessMaskBit::RW;
    return AccessMaskBit::None;
  }

  void addTargetSpecificPasses(PassManagerWrapper &PM) const override {}

  void addTargetLegalizationPasses(PassManagerWrapper &PM) const override {
    PM.add(createRISCVExpandPseudoPass());
    PM.add(createRISCVExpandAtomicPseudoPass());
  }

  void initializeTargetPasses() const override {
    auto *PM = PassRegistry::getPassRegistry();
    initializeRISCVExpandPseudoPass(*PM);
    initializeRISCVExpandAtomicPseudoPass(*PM);
  }

  unsigned countAddrsToGenerate(unsigned Opcode) const override {
    if (isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode) ||
        isLrInstr(Opcode) || isScInstr(Opcode))
      return 1;
    return 0;
  }

  std::pair<AddressParts, MemAddresses>
  breakDownAddr(AddressInfo AddrInfo, const MachineInstr &MI, unsigned AddrIdx,
                GeneratorContext &GC) const override {
    auto Opcode = MI.getOpcode();
    assert((isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode) ||
            isLrInstr(Opcode) || isScInstr(Opcode)) &&
           "Requested addr calculation for unsupported instruction");
    assert(AddrIdx == 0 && "RISC-V supports only one address per instruction");

    const auto &TM = GC.getLLVMState().getTargetMachine();
    if (isAtomicAMO(Opcode) || isLrInstr(Opcode) || isScInstr(Opcode) ||
        isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
        isRVVUnitStrideSegLoadStore(Opcode) || isRVVWholeRegLoadStore(Opcode) ||
        isRVVUnitStrideMaskLoadStore(Opcode)) {
      auto &State = GC.getLLVMState();
      auto &RI = State.getRegInfo();
      const auto &AddrReg = getMemOperand(MI);
      auto AddrValue = AddrInfo.Address;
      const auto &ST = GC.getSubtarget<RISCVSubtarget>();
      auto Part = AddressPart{AddrReg, APInt(ST.getXLen(), AddrValue), RI};

      if (isAtomicAMO(Opcode) || isLrInstr(Opcode) || isScInstr(Opcode) ||
          isRVVWholeRegLoadStore(Opcode))
        return std::make_pair<AddressParts, MemAddresses>(
            {std::move(Part)}, {uintToTargetXLen(is64Bit(TM), AddrValue)});

      assert(isRVV(Opcode));
      auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
      auto VL = TgtCtx.getVL(*MI.getParent());
      if (isRVVUnitStrideMaskLoadStore(Opcode))
        // RVV unit-stride mask instructions operate similarly to unmasked
        // byte loads or stores (EEW=8), except that the effective vector
        // length is evl=ceil(vl/8) (i.e. EMUL=1)
        VL = divideCeil(VL, 8);

      auto EEW = getDataElementWidth(Opcode);
      auto Addresses =
          generateStridedMemAccesses(AddrValue, EEW, VL, is64Bit(TM));
      return std::make_pair<AddressParts, MemAddresses>({std::move(Part)},
                                                        std::move(Addresses));
    }
    if (isRVVStridedLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode))
      return breakDownAddrForRVVStrided(AddrInfo, MI, GC, is64Bit(TM));
    if (isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode))
      return breakDownAddrForRVVIndexed(AddrInfo, MI, GC, is64Bit(TM));
    return breakDownAddrForInstrWithImmOffset(AddrInfo, MI, GC, is64Bit(TM));
  }

  unsigned getWriteValueSequenceLength(APInt Value, MCRegister Register,
                                       GeneratorContext &GC) const override {
    if (RISCV::VRRegClass.contains(Register))
      report_fatal_error("Not implemented for RVV regs yet");

    return getIntMatInstrSeq(Value, GC).size();
  }

  void writeValueToReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       APInt Value, unsigned DstReg, RegPoolWrapper &RP,
                       GeneratorContext &GC) const override {
    if (RISCV::VRRegClass.contains(DstReg)) {
      rvvWriteValue(MBB, Ins, GC, Value, DstReg, RP);
      return;
    }

    if (RISCV::FPR64RegClass.contains(DstReg) ||
        RISCV::FPR32RegClass.contains(DstReg) ||
        RISCV::FPR16RegClass.contains(DstReg)) {
      writeValueToFPReg(MBB, Ins, Value, DstReg, RP, GC);
      return;
    }

    auto &State = GC.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    unsigned SrcReg = RISCV::X0;
    auto ISeq = getIntMatInstrSeq(Value, GC);

    for (auto &Inst : ISeq) {
      auto MIB = getSupportInstBuilder(MBB, Ins, State.getCtx(),
                                       InstrInfo.get(Inst.getOpcode()), DstReg);
      switch (Inst.getOpndKind()) {
      case RISCVMatInt::Imm:
        MIB.addImm(Inst.getImm());
        break;
      case RISCVMatInt::RegX0:
        MIB.addReg(SrcReg).addReg(RISCV::X0);
        break;
      case RISCVMatInt::RegReg:
        MIB.addReg(SrcReg).addReg(SrcReg);
        break;
      case RISCVMatInt::RegImm:
        MIB.addReg(SrcReg).addImm(Inst.getImm());
        break;
      }
      SrcReg = DstReg;
    }
  }

  void loadRegFromAddrInReg(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Ins, MCRegister AddrReg,
                            MCRegister Reg, GeneratorContext &GC) const {
    assert(RISCV::GPRRegClass.contains(AddrReg) &&
           "Expected address register be GPR");
    auto &State = GC.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();

    if (RISCV::GPRRegClass.contains(Reg)) {
      const auto &ST = GC.getSubtarget<RISCVSubtarget>();
      auto LoadOp = ST.getXLen() == 32 ? RISCV::LW : RISCV::LD;
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(LoadOp), Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR16RegClass.contains(Reg)) {
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::FLH), Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR32RegClass.contains(Reg)) {
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::FLW), Reg)
          .addReg(AddrReg)
          .addImm(0);

    } else if (RISCV::FPR64RegClass.contains(Reg)) {
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::FLD), Reg)
          .addReg(AddrReg)
          .addImm(0);

    } else if (RISCV::VRRegClass.contains(Reg)) {
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::VL1RE8_V), Reg)
          .addReg(AddrReg);
    } else {
      report_fatal_error(
          Twine("Cannot generate load from memory for register ") +
              std::to_string(Reg),
          false);
    }
  }

  void loadRegFromAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                       uint64_t Addr, MCRegister Reg, RegPoolWrapper &RP,
                       GeneratorContext &GC) const override {
    // Form address in scratch register.
    auto &State = GC.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    auto XScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                     RP, MBB, AccessMaskBit::SRW);
    writeValueToReg(MBB, Ins, APInt(getRegBitWidth(XScratchReg, GC), Addr),
                    XScratchReg, RP, GC);
    loadRegFromAddrInReg(MBB, Ins, XScratchReg, Reg, GC);
  }

  void storeRegToAddrInReg(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator Ins, MCRegister AddrReg,
                           MCRegister Reg, GeneratorContext &GC,
                           unsigned BytesToWrite = 0) const {
    assert(RISCV::GPRRegClass.contains(AddrReg) &&
           "Expected address register be GPR");
    auto &State = GC.getLLVMState();
    auto &Ctx = State.getCtx();
    const auto &InstrInfo = State.getInstrInfo();

    if (RISCV::GPRRegClass.contains(Reg)) {
      auto StoreOp = getStoreOpcode(BytesToWrite ? BytesToWrite * RISCV_CHAR_BIT
                                                 : getRegBitWidth(Reg, GC));
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(StoreOp))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR32RegClass.contains(Reg) ||
               RISCV::FPR16RegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, GC));
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::FSW))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR64RegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, GC));
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::FSD))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::VRRegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, GC));
      getSupportInstBuilder(MBB, Ins, Ctx, InstrInfo.get(RISCV::VS1R_V))
          .addReg(Reg)
          .addReg(AddrReg);
    } else {
      report_fatal_error(
          Twine("Cannot generate store to memory for register ") + Twine(Reg),
          false);
    }
  }

  void storeRegToAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                      uint64_t Addr, MCRegister Reg, RegPoolWrapper &RP,
                      GeneratorContext &GC,
                      unsigned BytesToWrite) const override {
    auto &State = GC.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    RP.addReserved(MBB, Reg);
    auto ScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                    RP, MBB, AccessMaskBit::SRW);
    auto XRegBitSize = getRegBitWidth(ScratchReg, GC);

    writeValueToReg(MBB, Ins, APInt(XRegBitSize, Addr), ScratchReg, RP, GC);
    storeRegToAddrInReg(MBB, Ins, ScratchReg, Reg, GC, BytesToWrite);
  }

  void storeValueToAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                        uint64_t Addr, APInt Value, RegPoolWrapper &RP,
                        GeneratorContext &GC) const override {
    auto &State = GC.getLLVMState();
    auto &RI = State.getRegInfo();

    Register RegForValue;
    auto ValueRegBitSize = Value.getBitWidth();
    if (ValueRegBitSize > (Reg8Bytes * RISCV_CHAR_BIT))
      RegForValue = RP.getAvailableRegister(
          "to write value", RI, RI.getRegClass(RISCV::VRRegClassID), MBB,
          [](unsigned Reg) { return Reg == RISCV::V0; }, AccessMaskBit::SRW);
    else {
      RegForValue = getNonZeroReg("to write value", RI,
                                  RI.getRegClass(RISCV::GPRRegClassID), RP, MBB,
                                  AccessMaskBit::SRW);
      if (ValueRegBitSize > getRegBitWidth(RegForValue, GC))
        snippy::fatal(State.getCtx(), "Selfcheck error ",
                      "selfcheck is not implemented for rv32 with D ext");
    }
    writeValueToReg(MBB, Ins, Value.zext(getRegBitWidth(RegForValue, GC)),
                    RegForValue, RP, GC);
    assert(ValueRegBitSize % RISCV_CHAR_BIT == 0);
    storeRegToAddr(MBB, Ins, Addr, RegForValue, RP, GC,
                   ValueRegBitSize / RISCV_CHAR_BIT);
  }

  size_t getAccessSize(unsigned Opcode, GeneratorContext &GC,
                       const MachineBasicBlock &MBB) const {
    assert(countAddrsToGenerate(Opcode) &&
           "Requested access size calculation, but instruction does not access "
           "memory");

    if (!isRVV(Opcode))
      return getDataElementWidth(Opcode);

    auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
    auto VL = TgtCtx.getVL(MBB);
    if (VL == 0)
      return 0;

    if (isRVVUnitStrideMaskLoadStore(Opcode))
      // RVV unit-stride mask instructions operate similarly to unmasked byte
      // loads or stores (EEW=8), except that the effective vector length is
      // evl=ceil(vl/8) (i.e. EMUL=1)
      VL = divideCeil(VL, 8);

    auto SEW = TgtCtx.getSEW(MBB);
    auto VLENB = TgtCtx.getVLENB();
    auto AccessSize =
        getDataElementWidth(Opcode, static_cast<unsigned>(SEW), VLENB);
    auto NFields = 1u;
    if (isRVVUnitStrideSegLoadStore(Opcode) ||
        isRVVStridedSegLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode))
      NFields = getNumFields(Opcode);

    unsigned Stride = 0;
    if (isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
        isRVVUnitStrideSegLoadStore(Opcode) ||
        isRVVUnitStrideMaskLoadStore(Opcode))
      // We treat RVV unit-stride instructions as one consecutive memory access.
      Stride = AccessSize * NFields;

    return AccessSize * NFields + Stride * (VL - 1);
  }

  virtual std::tuple<size_t, size_t>
  getAccessSizeAndAlignment(unsigned Opcode, GeneratorContext &GC,
                            const MachineBasicBlock &MBB) const override {
    unsigned SEW = 0;
    const auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
    if (TgtCtx.hasActiveRVVMode(MBB))
      SEW = static_cast<unsigned>(TgtCtx.getSEW(MBB));
    return {getAccessSize(Opcode, GC, MBB), getLoadStoreAlignment(Opcode, SEW)};
  }

  std::vector<Register>
  excludeFromMemRegsForOpcode(unsigned Opcode) const override {
    if (isCLoadStore(Opcode) || isCFPLoadStore(Opcode)) {
      std::vector<Register> Result;
      Result.reserve(32);
      if (isCSPRelativeLoadStore(Opcode) || isCFPSPRelativeLoadStore(Opcode)) {
        copy_if(getAddrRegClass(), std::back_inserter(Result),
                [](Register Reg) { return Reg != RISCV::X2; });
      } else {
        copy_if(getAddrRegClass(), std::back_inserter(Result),
                [](Register Reg) {
                  return !is_contained(RISCV::GPRCRegClass, Reg);
                });
      }
      return Result;
    }
    return {RISCV::X0};
  }

  std::vector<Register> excludeRegsForOperand(const MCRegisterClass &RC,
                                              const GeneratorContext &GC,
                                              const MCInstrDesc &InstrDesc,
                                              unsigned Operand) const override {
    if (NoMaskModeForRVV)
      return {RISCV::V0, RISCV::V0M8, RISCV::V0M4, RISCV::V0M2};

    auto Opcode = InstrDesc.getOpcode();
    if (!isRVV(Opcode))
      return {};
    if (RC.getID() == RISCV::VMV0RegClass.getID())
      return {};
    return {RISCV::V0, RISCV::V0M8, RISCV::V0M4, RISCV::V0M2};
  }

  void excludeRegsForOpcode(unsigned Opcode, RegPoolWrapper &RP,
                            GeneratorContext &GC,
                            const MachineBasicBlock &MBB) const override {
    if (!isRVV(Opcode))
      return;

    auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
    if (!TgtCtx.hasActiveRVVMode(MBB))
      return;

    auto [Multiplier, IsFractional] = decodeVLMUL(TgtCtx.getLMUL(MBB));
    // Use EMUL instead of LMUL for the instructions listed below.
    if (isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
        isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedLoadStore(Opcode) ||
        isRVVStridedSegLoadStore(Opcode)) {
      auto LMUL = TgtCtx.getLMUL(MBB);
      auto SEW = TgtCtx.getSEW(MBB);
      auto EEW = getDataElementWidth(Opcode) * CHAR_BIT;
      auto EMUL = computeEMUL(static_cast<unsigned>(SEW), EEW, LMUL);

      std::tie(Multiplier, IsFractional) = RISCVVType::decodeVLMUL(EMUL);
    }

    if (isRVVUnitStrideSegLoadStore(Opcode) ||
        isRVVStridedSegLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) {
      // For segment load/stores #fields = #regs groups to use. So, we should
      // reserve (#fields - 1) last vregs groups.
      auto NFields = getNumFields(Opcode);
      auto RegNumMult = IsFractional ? 1u : Multiplier;
      assert(RegNumMult * NFields <= 8 && "RVV spec 7.8: EMUL * NFIELDS <= 8");
      for (auto i = 0u; i < (NFields - 1) * RegNumMult; ++i)
        // TODO: disallow reading for stores, writing for loads.
        RP.addReserved(RISCV::V31 - i);
    }

    if (isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) {
      auto LMUL = TgtCtx.getLMUL(MBB);
      auto SEW = static_cast<unsigned>(TgtCtx.getSEW(MBB));
      auto EIEW = getIndexElementWidth(Opcode);
      if (SEW < EIEW) {
        auto EMUL = computeEMUL(static_cast<unsigned>(SEW), EIEW, LMUL);
        std::tie(Multiplier, IsFractional) = RISCVVType::decodeVLMUL(EMUL);
      }
    }

    if ((isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
         isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) &&
        !IsFractional) {
      auto LMUL = TgtCtx.getLMUL(MBB);
      auto SEW = static_cast<unsigned>(TgtCtx.getSEW(MBB));
      auto EMUL = computeEMUL(SEW, SEW * 2u, LMUL);
      std::tie(Multiplier, IsFractional) = RISCVVType::decodeVLMUL(EMUL);
    }

    if (isRVVGather16(Opcode)) {
      auto LMUL = TgtCtx.getLMUL(MBB);
      auto SEW = static_cast<unsigned>(TgtCtx.getSEW(MBB));
      auto EEW = 16u;
      if (EEW > SEW) {
        auto EMUL = computeEMUL(SEW, EEW, LMUL);
        std::tie(Multiplier, IsFractional) = RISCVVType::decodeVLMUL(EMUL);
      }
    }

    // If LMUL > 1, we need to form register groups. Each group will have LMUL
    // consequent registers and starts at register number that is the multiple
    // of LMUL.
    if (IsFractional || Multiplier == 1)
      return;

    // FIXME: RVV instruction operands might have different restrictions. For
    // example, VZEXT's dst and src registers have different EMULs which leads
    // to different register group restrictions. Now we choose the strongest
    // existed rule for the instruction with the given VType and use it for all
    // operands. As a future improvement, we can choose rules for each operand
    // separately.
    for (auto i = 0u; i < RISCV::VRRegClass.getNumRegs(); ++i)
      if (i % Multiplier != 0) {
        auto VReg = regIndexToMCReg(i, RegStorageType::VReg, GC);
        RP.addReserved(VReg);
      }
  }

  std::vector<Register> includeRegs(const MCRegisterClass &RC) const override {
    if (RC.getID() == RISCV::VMV0RegClass.getID())
      return {RISCV::NoRegister};
    return {};
  }

  // Reserve a register (register group) that has already been used as the
  // destination or memory if it is needed for the given opcode.
  // (changes in the GeneratorContext may cause problems
  //  in operands pregeneration)
  void reserveRegsIfNeeded(unsigned Opcode, bool isDst, bool isMem,
                           Register Reg, RegPoolWrapper &RP,
                           GeneratorContext &GC,
                           const MachineBasicBlock &MBB) const override {
    // For vector indexed segment loads, the destination vector register groups
    // cannot overlap the source vector register group (specifed by vs2), else
    // the instruction encoding is reserved.
    if ((isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) &&
        isDst) {
      assert(RISCV::VRRegClass.contains(Reg) &&
             "Dst reg in rvv indexed load/store instruction must be vreg");
      auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();

      auto [Mult, Fractional] = decodeVLMUL(TgtCtx.getLMUL(MBB));
      // Register group here means a register group formed by LMUL multiplied by
      // NFields for segment which is confusing.
      auto DstRegGroupSize = Fractional ? 1u : Mult;
      if (isRVVIndexedSegLoadStore(Opcode))
        DstRegGroupSize *= getNumFields(Opcode);
      for (auto i = 0u; i < DstRegGroupSize; ++i)
        RP.addReserved(Reg + i);
    }
    // Addr base reg and stride reg shouldn't match as it's unlikely that a
    // memory scheme will allow such accesses: `base + n * stride` where n = [0;
    // VLMAX], base reg == stride reg.
    if ((isRVVStridedLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) &&
        isMem)
      RP.addReserved(Reg);

    // For opcodes below SPEC forbids overlapping of dst and src register
    // groups.
    if (isRVVSetFirstMaskBit(Opcode) && isDst)
      RP.addReserved(Reg);
    if (isRVVIota(Opcode) && isDst)
      RP.addReserved(Reg);
    if (isRVVCompress(Opcode) && isDst)
      RP.addReserved(Reg);
    if ((isRVVSlide1Up(Opcode) || isRVVSlideUp(Opcode)) && isDst)
      RP.addReserved(Reg);
    if (isRVVExt(Opcode) && isDst)
      RP.addReserved(Reg);
    if ((isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode)) && isDst)
      RP.addReserved(Reg);
    if ((isRVVGather(Opcode) || isRVVGather16(Opcode)) && isDst)
      RP.addReserved(Reg);
  }

  const TargetRegisterClass &getAddrRegClass() const override {
    return RISCV::GPRRegClass;
  }

  unsigned getAddrRegLen(const TargetMachine &TM) const override {
    return is64Bit(TM) ? 64u : 32u;
  }

  bool canUseInMemoryBurstMode(unsigned Opcode) const override {
    return isLoadStore(Opcode) || isFPLoadStore(Opcode) ||
           isAtomicAMO(Opcode) || isCLoadStore(Opcode) ||
           isCFPLoadStore(Opcode) || isFence(Opcode);
  }

  StridedImmediate getImmOffsetRangeForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    auto Opcode = InstrDesc.getOpcode();
    assert((isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode)) &&
           "Expected memory access instruction");
    if (isLoadStore(Opcode) || isFPLoadStore(Opcode)) {
      assert(getMemOperandIdx(InstrDesc) + 1 < InstrDesc.getNumOperands());
      assert(
          InstrDesc.operands()[getMemOperandIdx(InstrDesc) + 1].OperandType ==
          RISCVOp::OPERAND_SIMM12);
      return StridedImmediate(APInt::getSignedMinValue(12).getSExtValue(),
                              APInt::getSignedMaxValue(12).getSExtValue(), 1);
    }
    if (isCLoadStore(Opcode) || isCFPLoadStore(Opcode)) {
      auto ImmOpType =
          InstrDesc.operands()[getMemOperandIdx(InstrDesc) + 1].OperandType;
      unsigned BitWidth = 0;
      unsigned ZeroBits = 0;
      switch (ImmOpType) {
      default:
        llvm_unreachable("Unknown ImmOpType");
        // Base load/store variants
      case RISCVOp::OPERAND_UIMM7_LSB00: {
        BitWidth = 7;
        ZeroBits = 2;
      } break;
      case RISCVOp::OPERAND_UIMM8_LSB00: {
        BitWidth = 8;
        ZeroBits = 2;
      } break;
        // Loads/stores relative to the stack pointer
      case RISCVOp::OPERAND_UIMM8_LSB000: {
        BitWidth = 8;
        ZeroBits = 3;
      } break;
      case RISCVOp::OPERAND_UIMM9_LSB000: {
        BitWidth = 9;
        ZeroBits = 3;
      } break;
      }
      return StridedImmediate(
          0, APInt::getMaxValue(BitWidth - ZeroBits).getZExtValue() << ZeroBits,
          1 << ZeroBits);
    }
    return StridedImmediate(0, 0, 0);
  }

  size_t getAccessSize(unsigned Opcode) const override {
    assert(isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode));
    // FIXME: To support RVV we must pass SEW and VLENB to
    // getDataElementWidth. But SEW and VLENB depend on the position, so
    // don't support it right now.
    assert(!isRVVExt(Opcode) &&
           "RVV opcodes are not supported in getAccessSize function");
    return getDataElementWidth(Opcode);
  }

  bool isCall(unsigned Opcode) const override { return snippy::isCall(Opcode); }

private:
  SmallVector<SectionDesc, 3> ReservedRanges;

  void rvvWriteValue(MachineBasicBlock &MBB, MachineBasicBlock::iterator Ins,
                     GeneratorContext &GC, APInt Value, unsigned DstReg,
                     RegPoolWrapper &RP) const;

  void rvvWriteValueUsingXReg(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator Ins,
                              GeneratorContext &GC, APInt Value,
                              unsigned DstReg, RegPoolWrapper &RP) const;

  void writeValueToFPReg(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator Ins, APInt Value,
                         unsigned DstReg, RegPoolWrapper &RP,
                         GeneratorContext &GC) const;

  // NOTE: DesiredOpcode is expected to be any mode changing opcode
  // (RISCV::VSETVL, RISCV::VSETVLI, RISCV::VSETIVLI) or
  // RISCV::INSTRUCTION_LIST_END to indicate the user does not really care
  // which instruction to use. In case of the latter the implementaion is free
  // to chose any suitable opcode
  void rvvGenerateModeSwitchAndUpdateContext(
      const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
      GeneratorContext &GC, MachineBasicBlock::iterator Ins,
      unsigned DesiredOpcode = RISCV::INSTRUCTION_LIST_END) const;

  void generateRVVModeUpdate(
      const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
      GeneratorContext &GC, const RVVConfiguration &Config,
      const RVVConfigurationInfo::VLVM &VLVM, MachineBasicBlock::iterator Ins,
      const RVVModeInfo *PendingRVVMode = nullptr,
      unsigned DesiredOpcode = RISCV::INSTRUCTION_LIST_END) const;

  void generateV0MaskUpdate(const RVVConfigurationInfo::VLVM &VLVM,
                            MachineBasicBlock::iterator Ins,
                            MachineBasicBlock &MBB, GeneratorContext &GC,
                            const MCInstrInfo &InstrInfo,
                            bool IsLegalConfiguration) const;

  void updateRVVConfig(const MachineInstr &MI, GeneratorContext &GC,
                       MachineBasicBlock::iterator Ins) const;

  // NOTE: generateVSET* functions are expected to be called by
  // generateRVVModeUpdate only
  void generateVSETIVLI(const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
                        GeneratorContext &GC, unsigned VTYPE, unsigned VL,
                        bool SupportMarker,
                        MachineBasicBlock::iterator Ins) const;

  void generateVSETVLI(const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
                       GeneratorContext &GC, unsigned VTYPE, unsigned VL,
                       const RVVModeInfo *PendingMode, bool SupportMarker,
                       MachineBasicBlock::iterator Ins) const;

  void generateVSETVL(const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB,
                      GeneratorContext &GC, unsigned VTYPE, unsigned VL,
                      const RVVModeInfo *PendingMode, bool SupportMarker,
                      MachineBasicBlock::iterator Ins) const;
};

void SnippyRISCVTarget::writeValueToFPReg(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator Ins,
                                          APInt Value, unsigned DstReg,
                                          RegPoolWrapper &RP,
                                          GeneratorContext &GC) const {
  assert(RISCV::FPR64RegClass.contains(DstReg) ||
         RISCV::FPR32RegClass.contains(DstReg) ||
         RISCV::FPR16RegClass.contains(DstReg));

  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  auto &State = GC.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();

  unsigned FMVOpc = RISCV::FMV_W_X;
  if (RISCV::FPR64RegClass.contains(DstReg))
    FMVOpc = RISCV::FMV_D_X;
  else if (RISCV::FPR16RegClass.contains(DstReg))
    FMVOpc = RISCV::FMV_H_X;

  auto NumBits = Value.getBitWidth();
  if (ST.getXLen() > NumBits) {
    Value = Value.zext(ST.getXLen());
    Value.setBitsFrom(NumBits);
  } else if (ST.getXLen() < NumBits) {
    // FIXME: This may occur when D extension is enabled on rv32. It's a legal
    // combination, though we do not have real scenarios for it. So, leaving
    // this part unimplemented.
    report_fatal_error(
        "Cannot write value to a FP register as it doesn't fit in GRP.", false);
  }

  auto &RI = State.getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto ScratchReg = getNonZeroReg("scratch register for writing FP register",
                                  RI, RegClass, RP, MBB, AccessMaskBit::SRW);
  writeValueToReg(MBB, Ins, Value, ScratchReg, RP, GC);
  getSupportInstBuilder(MBB, Ins, State.getCtx(), InstrInfo.get(FMVOpc), DstReg)
      .addReg(ScratchReg);
}

void SnippyRISCVTarget::rvvWriteValueUsingXReg(MachineBasicBlock &MBB,
                                               MachineBasicBlock::iterator Ins,
                                               GeneratorContext &GC,
                                               APInt Value, unsigned DstReg,
                                               RegPoolWrapper &RP) const {
  auto &State = GC.getLLVMState();
  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  const auto &InstrInfo = State.getInstrInfo();
  assert(ST.hasStdExtV());

  auto SEW = 64u;
  auto VL = 2u;

  bool TA = false;
  bool MA = false;

  // FIXME: We may want to write 64 bits value when VL = 1 and SEW = 32b, so
  // we have to change vtype to be sure we do right things. This is a
  // temporary solution and is going to be replaced with initialization by
  // memory operations.
  auto [RVVConfig, VLVM] =
      constructRVVModeWithVMReset(RISCVII::VLMUL::LMUL_1, VL, SEW, TA, MA);

  generateRVVModeUpdate(InstrInfo, MBB, GC, RVVConfig, VLVM, Ins);

  // Use non-reserved reg as scratch.
  auto &RI = State.getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto XScratchReg = getNonZeroReg("scratch register", RI, RegClass, RP, MBB,
                                   AccessMaskBit::SRW);
  assert(RISCV::VRRegClass.contains(DstReg));

  // FIXME: We must set undef flag only when we do initialization. In all
  // other cases it's not quite right to use it. However, I expect the whole
  // this function to be a temporary solution, so it shouldn't be a big
  // problem.
  unsigned RegFlags = RegState::Undef;
  for (unsigned Idx = 0; Idx < VL; ++Idx) {
    auto EltValue = Value.extractBitsAsZExtValue(SEW, Idx * SEW);
    writeValueToReg(MBB, Ins, APInt(SEW, EltValue), XScratchReg, RP, GC);
    getSupportInstBuilder(MBB, Ins, State.getCtx(),
                          InstrInfo.get(RISCV::VSLIDE1DOWN_VX), DstReg)
        .addReg(DstReg, RegFlags)
        .addReg(XScratchReg)
        // Disable masking
        .addReg(RISCV::NoRegister);
    RegFlags = 0;
  }
  const auto &RGC = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  if (RGC.hasActiveRVVMode(MBB)) {
    const auto &CurRVVMode = RGC.getActiveRVVMode(MBB);
    generateRVVModeUpdate(InstrInfo, MBB, GC, *CurRVVMode.Config,
                          CurRVVMode.VLVM, Ins);
  }
}
void SnippyRISCVTarget::rvvWriteValue(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator Ins,
                                      GeneratorContext &GC, APInt Value,
                                      unsigned DstReg,
                                      RegPoolWrapper &RP) const {
  // FIXME for V0 we can only use global variables for initialization
  if (!InitVRegsFromMemory.getValue() && DstReg != RISCV::V0) {
    rvvWriteValueUsingXReg(MBB, Ins, GC, Value, DstReg, RP);
    return;
  }

  assert(GC.getSubtarget<RISCVSubtarget>().hasStdExtV());
  auto &GP = GC.getGlobalsPool();
  const auto *GV =
      GP.createGV(Value, /* Alignment */ Reg16Bytes,
                  /* Linkage */ GlobalValue::InternalLinkage,
                  /* Name */ "global",
                  /* Reason */ "This is needed for updating of RVV register.");

  auto GVAddr = GP.getGVAddress(GV);
  loadRegFromAddr(MBB, Ins, GVAddr, DstReg, RP, GC);
  GC.NotifyMemUpdate(GVAddr, Value);
}

void SnippyRISCVTarget::updateRVVConfig(const MachineInstr &MI,
                                        GeneratorContext &GC,
                                        MachineBasicBlock::iterator Ins) const {
  if (MI.getNumDefs() == 0)
    return;
  if (!isRVV(MI.getDesc().getOpcode()))
    return;
  assert(Ins.isValid());
  // Ins may points to end().
  // To get changeable MBB, decrease Ins pos by one.
  // This is always valid, as we create at least one instruction MI.
  auto &MBB = *(std::prev(Ins))->getParent();
  auto &RGC = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto &State = GC.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &VUInfo = RGC.getVUConfigInfo();
  const auto &RVVMode = RGC.getActiveRVVMode(MBB);
  for (auto &&Def : MI.defs()) {
    if (!Def.isReg())
      continue;
    auto &&Reg = Def.getReg();
    if (Reg != RISCV::V0)
      continue;
    // We have write to V0. Update V0 Mask with the value from config.
    // FIXME: basically, we can be better, and check value from Interpreter...
    auto NewVLVM = VUInfo.updateVM(*RVVMode.Config, RVVMode.VLVM);
    generateV0MaskUpdate(NewVLVM, Ins, MBB, GC, InstrInfo,
                         RVVMode.Config->IsLegal);
    // We change only V0 here...
    RGC.updateActiveRVVMode(NewVLVM, *RVVMode.Config, MBB);
  }
}

void SnippyRISCVTarget::instructionPostProcess(
    MachineInstr &MI, GeneratorContext &GC,
    MachineBasicBlock::iterator Ins) const {
  updateRVVConfig(MI, GC, Ins);
}

void SnippyRISCVTarget::rvvGenerateModeSwitchAndUpdateContext(
    const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB, GeneratorContext &GC,
    MachineBasicBlock::iterator Ins, unsigned DesiredOpcode) const {
  auto &RGC = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &VUInfo = RGC.getVUConfigInfo();
  // TODO: likely, we should return a pair
  const auto &NewRvvCFG = VUInfo.selectConfiguration();
  const auto &NewVLVM =
      VUInfo.selectVLVM(NewRvvCFG, DesiredOpcode == RISCV::VSETIVLI);

  const RVVModeInfo *PendingRVVModePtr = nullptr;
  RVVModeInfo PendingMode;
  // NOTE: active RVV mode is optional, so we don't always provide one
  if (RGC.hasActiveRVVMode(MBB)) {
    PendingMode = RGC.getActiveRVVMode(MBB);
    PendingRVVModePtr = &PendingMode;
  }
  generateRVVModeUpdate(InstrInfo, MBB, GC, NewRvvCFG, NewVLVM, Ins,
                        PendingRVVModePtr, DesiredOpcode);
  RGC.updateActiveRVVMode(NewVLVM, NewRvvCFG, MBB);
}

static unsigned
selectDesiredModeChangeInstruction(RVVModeChangeMode Preference, unsigned VL,
                                   const RISCVGeneratorContext &TargetContext) {
  switch (Preference) {
  case RVVModeChangeMode::MC_ANY: {
    const auto &ModeChangeInfo =
        TargetContext.getVUConfigInfo().getModeChangeInfo();
    std::array<double, 3> VsetvlProb = {
        ModeChangeInfo.WeightVSETVL,
        ModeChangeInfo.WeightVSETVLI,
        // NOTE: VSETIVLI is limited to VL kMaxVLForVSETIVLI so we exclude
        // this mode-changing instruction for cases when it can't be used
        (VL > kMaxVLForVSETIVLI) ? 0.0 : ModeChangeInfo.WeightVSETIVLI,
    };
    // NOTE: this, probably, should be an assert. However, we don't have proper
    // checks at the configuration phase
    if (std::all_of(VsetvlProb.begin(), VsetvlProb.end(),
                    [](const auto &P) { return P <= 0.0; }))
      report_fatal_error(
          "The specified restrictions on VSET* instructions "
          "do not allow to produce VL of " +
              Twine(VL) +
              ". Please, adjust the histogram or change the set of "
              "reachable RVV configurations",
          false);

    DiscreteGeneratorInfo<unsigned, std::array<unsigned, 3>> Gen(
        {RISCV::VSETVL, RISCV::VSETVLI, RISCV::VSETIVLI}, VsetvlProb);
    return Gen();
  }
  case RVVModeChangeMode::MC_VSETIVLI:
    if (VL > kMaxVLForVSETIVLI)
      report_fatal_error("cannot select VSETIVLI as mode changing instruction "
                         " for VL greater than 31",
                         false);
    return RISCV::VSETIVLI;
  case RVVModeChangeMode::MC_VSETVLI:
    return RISCV::VSETVLI;
  case RVVModeChangeMode::MC_VSETVL:
    return RISCV::VSETVL;
  }
  llvm_unreachable("unexpected RVV mode change preference");
}

void SnippyRISCVTarget::generateV0MaskUpdate(
    const RVVConfigurationInfo::VLVM &VLVM, MachineBasicBlock::iterator Ins,
    MachineBasicBlock &MBB, GeneratorContext &GC, const MCInstrInfo &InstrInfo,
    bool IsLegalConfiguration) const {
  // We do not interested in any V0 mask update
  if (NoMaskModeForRVV)
    return;
  // FIXME: Now if RVVConfigutation is illegal we can update V0 only via whole
  // register load
  if (!IsLegalConfiguration)
    return;

  if (VLVM.VM.isAllOnes()) {
    LLVM_DEBUG(dbgs() << "Resetting mask instruction for mask:"
                      << toString(VLVM.VM, /* Radix */ 16, /* Signed */ false)
                      << "\n");
    generateRVVMaskReset(InstrInfo, MBB, Ins);
    return;
  }

  auto RP = GC.getRegisterPool();
  // Note: currently used load from memory instruction,
  // So real mask value width should be VLEN.
  auto &TgtCtx = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto VLEN = TgtCtx.getVLEN();
  auto WidenVM = VLVM.VM.zext(VLEN);
  LLVM_DEBUG(dbgs() << "Mask update with memory instruction for mask:"
                    << toString(WidenVM, /* Radix */ 16, /* Signed */ false)
                    << "\n");
  rvvWriteValue(MBB, Ins, GC, WidenVM, RISCV::V0, RP);
}

void SnippyRISCVTarget::generateVSETIVLI(
    const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB, GeneratorContext &GC,
    unsigned VTYPE, unsigned VL, bool SupportMarker,
    MachineBasicBlock::iterator Ins) const {
  auto &State = GC.getLLVMState();
  const auto &RI = State.getRegInfo();
  auto RP = GC.getRegisterPool();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto DstReg = RP.getAvailableRegister("VSETIVLI dst", RI, RegClass, MBB,
                                        SupportMarker ? AccessMaskBit::SRW
                                                      : AccessMaskBit::GRW);
  // TODO: eventually this should be an assert
  if (VL > kMaxVLForVSETIVLI)
    report_fatal_error("cannot set the desired VL " + Twine(VL) +
                           " since selected VSETIVLI does not support it",
                       false);
  auto MIB = getInstBuilder(SupportMarker, MBB, Ins, GC.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETIVLI));
  MIB.addDef(DstReg).addImm(VL).addImm(VTYPE);
}

void SnippyRISCVTarget::generateVSETVLI(
    const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB, GeneratorContext &GC,
    unsigned VTYPE, unsigned VL,
    [[maybe_unused]] const RVVModeInfo *PendingMode, bool SupportMarker,
    MachineBasicBlock::iterator Ins) const {
  // TODO 1: if VL is equal to VLMAX we can use X0 if DstReg is not zero
  // TODO 2: if VL is not changed, and DST is zero, scratch VL can be zero
  const auto &RI = GC.getLLVMState().getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto RP = GC.getRegisterPool();
  auto DstReg = RP.getAvailableRegister("for VSETVLI dst", RI, RegClass, MBB,
                                        SupportMarker ? AccessMaskBit::SRW
                                                      : AccessMaskBit::GRW);
  auto ScratchRegVL = getNonZeroReg("for VSETVLI VL", RI, RegClass, RP, MBB,
                                    AccessMaskBit::SRW);
  writeValueToReg(MBB, Ins,
                  APInt(GC.getSubtarget<RISCVSubtarget>().getXLen(), VL),
                  ScratchRegVL, RP, GC);
  auto MIB = getInstBuilder(SupportMarker, MBB, Ins, GC.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETVLI));
  MIB.addDef(DstReg);
  MIB.addReg(ScratchRegVL);
  MIB.addImm(VTYPE);
}

void SnippyRISCVTarget::generateVSETVL(
    const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB, GeneratorContext &GC,
    unsigned VTYPE, unsigned VL,
    [[maybe_unused]] const RVVModeInfo *PendingMode, bool SupportMarker,
    MachineBasicBlock::iterator Ins) const {
  // TODO 1: if VL is equal to VLMAX we can use X0 if DstReg is not zero
  // TODO 2: if VL is not changed, and DST is zero, scratch VL can be zero
  const auto &RI = GC.getLLVMState().getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto RP = GC.getRegisterPool();
  auto DstReg = RP.getAvailableRegister("for VSETVL dst", RI, RegClass, MBB,
                                        SupportMarker ? AccessMaskBit::SRW
                                                      : AccessMaskBit::GRW);
  const auto &ST = GC.getSubtarget<RISCVSubtarget>();
  // TODO: maybe just use GPRNoX0RegClassID class?
  auto [ScratchRegVL, ScratchRegVType] = RP.getNAvailableRegisters<2>(
      "registers for VSETVL VL and VType", RI, RegClass, MBB,
      /* Filter */ [](unsigned Reg) { return Reg == RISCV::X0; },
      AccessMaskBit::SRW);
  writeValueToReg(MBB, Ins, APInt(ST.getXLen(), VL), ScratchRegVL, RP, GC);
  RP.addReserved(ScratchRegVL);
  writeValueToReg(MBB, Ins, APInt(ST.getXLen(), VTYPE), ScratchRegVType, RP,
                  GC);
  auto MIB = getInstBuilder(SupportMarker, MBB, Ins, GC.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETVL));
  MIB.addDef(DstReg);
  MIB.addReg(ScratchRegVL);
  MIB.addReg(ScratchRegVType);
}

void SnippyRISCVTarget::generateRVVModeUpdate(
    const MCInstrInfo &InstrInfo, MachineBasicBlock &MBB, GeneratorContext &GC,
    const RVVConfiguration &Config, const RVVConfigurationInfo::VLVM &VLVM,
    MachineBasicBlock::iterator Ins, const RVVModeInfo *PendingRVVMode,
    unsigned DesiredOpcode) const {
  unsigned SEW = static_cast<unsigned>(Config.SEW);
  LLVM_DEBUG(dbgs() << "Emit RVV Mode Change: VL = " << VLVM.VL
                    << ", SEW = " << SEW << ", TA = " << Config.TailAgnostic
                    << ", MA = " << Config.MaskAgnostic << "\n");
  auto VTYPE = RISCVVType::encodeVTYPE(Config.LMUL, SEW, Config.TailAgnostic,
                                       Config.MaskAgnostic);
  auto &RGC = GC.getTargetContext().getImpl<RISCVGeneratorContext>();
  bool SupportMarker = false;
  if (DesiredOpcode == RISCV::INSTRUCTION_LIST_END) {
    DesiredOpcode = selectDesiredModeChangeInstruction(
        RVVModeChangePreferenceOpt, VLVM.VL, RGC);
    SupportMarker = true;
  } else {
    const auto &VUInfo = RGC.getVUConfigInfo();
    SupportMarker = VUInfo.isModeChangeArtificial();
  }

  switch (DesiredOpcode) {
  case RISCV::VSETIVLI:
    generateVSETIVLI(InstrInfo, MBB, GC, VTYPE, VLVM.VL, SupportMarker, Ins);
    break;
  case RISCV::VSETVLI:
    generateVSETVLI(InstrInfo, MBB, GC, VTYPE, VLVM.VL, PendingRVVMode,
                    SupportMarker, Ins);
    break;
  case RISCV::VSETVL:
    generateVSETVL(InstrInfo, MBB, GC, VTYPE, VLVM.VL, PendingRVVMode,
                   SupportMarker, Ins);
    break;
  default:
    llvm_unreachable("unexpected OpcodeRequested for generateRVVModeUpdate");
  }
  generateV0MaskUpdate(VLVM, Ins, MBB, GC, InstrInfo, Config.IsLegal);

  // TODO: update VXRM/VXSAT
}

static void dumpRvvConfigurationInfo(StringRef FilePath,
                                     const RVVConfigurationInfo &RVVCfg) {
  if (FilePath.empty()) {
    RVVCfg.print(outs());
    return;
  }

  auto ReportFileError = [FilePath](const std::error_code &EC) {
    report_fatal_error("could not create " + FilePath +
                           " for RVV config dump: " + EC.message(),
                       false);
  };
  // TODO: this code is a rather common pattern. Probably, it should be
  // factored-out to a separate function
  std::error_code EC;
  raw_fd_ostream OS(FilePath, EC);
  if (EC)
    ReportFileError(EC);

  RVVCfg.print(OS);

  if (OS.has_error())
    ReportFileError(OS.error());
}

std::unique_ptr<TargetGenContextInterface>
SnippyRISCVTarget::createTargetContext(const GeneratorContext &Ctx) const {
  auto RISCVCfg = RISCVConfigurationInfo::constructConfiguration(Ctx);

  if (DumpRVVConfigurationInfo.isSpecified())
    dumpRvvConfigurationInfo(DumpRVVConfigurationInfo.getValue(),
                             RISCVCfg.getVUConfig());

  return std::make_unique<RISCVGeneratorContext>(std::move(RISCVCfg));
}

std::unique_ptr<TargetConfigInterface>
SnippyRISCVTarget::createTargetConfig() const {
  return std::make_unique<RISCVConfigInterface>();
}

std::unique_ptr<SimulatorInterface>
SnippyRISCVTarget::createSimulator(llvm::snippy::DynamicLibrary &ModelLib,
                                   const SimulationConfig &Cfg,
                                   const TargetGenContextInterface *TgtGenCtx,
                                   RVMCallbackHandler *CallbackHandler,
                                   const TargetSubtargetInfo &SubTgt) const {
  const auto &Subtarget = static_cast<const RISCVSubtarget &>(SubTgt);
  unsigned VLENB = 0;
  if (Subtarget.hasStdExtV()) {
    assert(TgtGenCtx);
    const RISCVGeneratorContext *TgtCtx =
        static_cast<const RISCVGeneratorContext *>(TgtGenCtx);
    VLENB = TgtCtx->getVLEN();
  }

  return createRISCVSimulator(ModelLib, Cfg, CallbackHandler, Subtarget, VLENB,
                              !RISCVDisableMisaligned);
}

} // anonymous namespace

static SnippyTarget *getTheRISCVSnippyTarget() {
  static SnippyRISCVTarget Target;
  return &Target;
}

void InitializeRISCVSnippyTarget() {
  SnippyTarget::registerTarget(getTheRISCVSnippyTarget());
}

} // namespace snippy
} // namespace llvm
