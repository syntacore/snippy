//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/RegsReservedForLoop.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Generator/SnippyLoopInfo.h"

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "snippy/Simulator/RISCVRegTypes.h"
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
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"

#include <cstdint>

#include <functional>
#include <limits>
#include <numeric>
#include <variant>

#define DEBUG_TYPE "snippy-riscv"

#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/TargetRegistry.h"

// include isOpcodeAvailable.
#define GET_AVAILABLE_OPCODE_CHECKER
#include "RISCVGenInstrInfo.inc"
#undef GET_AVAILABLE_OPCODE_CHECKER

#define SNIPPY_RISCV_EXCLUDED_OPCODES_DEF
#include "RISCVGenerated.inc"
#undef SNIPPY_RISCV_EXCLUDED_OPCODES_DEF

namespace llvm {
#define GET_REGISTER_MATCHER
#include "RISCVGenAsmMatcher.inc"
namespace snippy {

enum class DisableMisalignedAccessMode { None, All, AtomicsOnly };

enum class RVVModeChangeMode {
  MC_ANY,
  MC_VSETIVLI,
  MC_VSETVLI,
  MC_VSETVL,
};

enum class RVVInitMode {
  Splats,
  Loads,
  Mixed,
  Slides,
};

enum class LoopControlLogicCompressionMode { On, Random, Off };

llvm::cl::OptionCategory
    SnippyRISCVOptions("llvm-snippy RISCV-specific options");

struct RVVInitModeEnumOption
    : public snippy::EnumOptionMixin<RVVInitModeEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(
        RVVInitMode::Splats, "splats",
        "Write value to xreg and move it to vreg using VMV.V.X instruction");
    Mapper.enumCase(
        RVVInitMode::Loads, "loads",
        "Load value from read-only section using VL1RE8.V instruction");
    Mapper.enumCase(RVVInitMode::Slides, "slides",
                    "Write value to xreg and slide it into vreg using "
                    "VSLIDE1DOWN.VX instruction. Repeat until vreg is filled");
    Mapper.enumCase(RVVInitMode::Mixed, "mixed",
                    "Use load for v0 and slides for v1-v31 initialization");
  }
};

static snippy::opt<RVVInitMode>
    RVVInitModeOpt("rvv-init-mode",
                   cl::desc("Controls how to initialize vector registers"),
                   RVVInitModeEnumOption::getClValues(),
                   cl::init(RVVInitMode::Mixed), cl::cat(SnippyRISCVOptions));

static snippy::opt<std::string> DumpRVVConfigurationInfo(
    "riscv-dump-rvv-config",
    cl::desc("Print verbose information about selected RVV configurations"),
    cl::init(""), cl::ValueOptional, cl::cat(SnippyRISCVOptions));

struct DisableMisalignedAccessEnumOption
    : public snippy::EnumOptionMixin<DisableMisalignedAccessEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(DisableMisalignedAccessMode::None, "false",
                    "enable misalign access");
    Mapper.enumCase(DisableMisalignedAccessMode::All, "",
                    "disable misaligned access for all loads/stores");
    Mapper.enumCase(DisableMisalignedAccessMode::All, "true",
                    "disable misaligned access for all loads/stores");
    Mapper.enumCase(DisableMisalignedAccessMode::All, "all",
                    "disable misaligned access for all loads/stores");
    Mapper.enumCase(DisableMisalignedAccessMode::AtomicsOnly, "atomics-only",
                    "disable misaligned access for atomic loads/stores only");
  }
};

static snippy::opt<DisableMisalignedAccessMode>
    RISCVDisableMisaligned("riscv-disable-misaligned-access",
                           DisableMisalignedAccessEnumOption::getClValues(),
                           cl::desc("disable misaligned load/store generation"),
                           cl::cat(SnippyRISCVOptions), cl::ValueOptional,
                           cl::init(DisableMisalignedAccessMode::All));

struct LoopControlLogicCompressionEnumOption
    : public snippy::EnumOptionMixin<LoopControlLogicCompressionEnumOption> {
  static void doMapping(EnumMapper &Mapper) {
    Mapper.enumCase(LoopControlLogicCompressionMode::On, "on",
                    "use compressed instruction as much as it possible");
    Mapper.enumCase(LoopControlLogicCompressionMode::Random, "random",
                    "compressed instructions may be used for loop counters");
    Mapper.enumCase(
        LoopControlLogicCompressionMode::Off, "off",
        "avoid using compressed instruction as much as it possible ");
  }
};

static snippy::opt<LoopControlLogicCompressionMode> LoopControlLogicCompression(
    "riscv-loop-control-logic-compression",
    LoopControlLogicCompressionEnumOption::getClValues(),
    cl::desc(
        "choose a policy for using compressed instruction for loop counters"),
    cl::cat(SnippyRISCVOptions), cl::ValueOptional,
    cl::init(LoopControlLogicCompressionMode::On));

// FIXME: Make Init*RegsFromMemory options target independent
static snippy::opt<bool> InitFRegsFromMemory(
    "riscv-init-fregs-from-memory",
    cl::desc("use preinitialized memory for initializing floating registers"),
    cl::cat(SnippyRISCVOptions));

static snippy::opt<bool>
    NoMaskModeForRVV("riscv-nomask-mode-for-rvv",
                     cl::desc("force use nomask for vector instructions"),
                     cl::Hidden, cl::cat(SnippyRISCVOptions), cl::init(false));

static snippy::opt<bool>
    SelfcheckRVV("enable-selfcheck-rvv",
                 cl::desc("turning on selfcheck for rvv instructions"),
                 cl::Hidden, cl::init(false));

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
    cl::desc("Preferences for RVV mode changing instructions (debug only)"),
    RVVModeChangeEnumOption::getClValues(), cl::Hidden,
    cl::init(RVVModeChangeMode::MC_ANY), cl::cat(SnippyRISCVOptions));

} // namespace snippy

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::RVVInitMode,
                                           snippy::RVVInitModeEnumOption)

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(
    snippy::DisableMisalignedAccessMode,
    snippy::DisableMisalignedAccessEnumOption)

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(
    snippy::LoopControlLogicCompressionMode,
    snippy::LoopControlLogicCompressionEnumOption)

LLVM_SNIPPY_OPTION_DEFINE_ENUM_OPTION_YAML(snippy::RVVModeChangeMode,
                                           snippy::RVVModeChangeEnumOption)

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
                                const SnippyProgramContext &ProgCtx) {
  assert(EEW && "Effective element width can not be zero");
  const auto &RISCVCtx =
      ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();

  if (isRVVUnitStrideSegLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) {
    // segment and non-indexed loads
    auto SEW = static_cast<unsigned>(RISCVCtx.getSEW(MBB));
    auto [EMUL, IsFractionEMUL] =
        computeDecodedEMUL(SEW, EEW, RISCVCtx.getLMUL(MBB));
    assert(RISCVVType::isValidLMUL(EMUL, IsFractionEMUL));
    return {EEW, EMUL};
  }

  auto [LMUL, IsFractional] = RISCVCtx.decodeVLMUL(RISCVCtx.getLMUL(MBB));
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
                             const SnippyProgramContext &ProgCtx) {
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
      ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
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
      getElemWidthAndGapForVectorLoad(Opcode, EEW, MBB, ProgCtx);

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
         isRVVWholeRegLoadStore(Opcode) ||
         isRVVUnitStrideMaskLoadStore(Opcode) || isZicbo(Opcode);
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

static inline unsigned
getIncOpcodeForLoopCounter(const InstructionGenerationContext &IGC) {
  if (!IGC.getSubtarget<RISCVSubtarget>().hasStdExtC())
    return RISCV::ADDI;

  if (LoopControlLogicCompression.getValue() ==
      LoopControlLogicCompressionMode::Random) {
    auto OpcodeChoice = RandEngine::genBool();
    return OpcodeChoice ? RISCV::C_ADDI : RISCV::ADDI;
  }
  return LoopControlLogicCompression.getValue() ==
                 LoopControlLogicCompressionMode::On
             ? RISCV::C_ADDI
             : RISCV::ADDI;
}

static RegStorageType regToStorage(Register Reg) {
  if ((RISCV::X0 <= Reg && Reg <= RISCV::X31) ||
      (RISCV::X0_W <= Reg && Reg <= RISCV::X31_W) ||
      (RISCV::X0_H <= Reg && Reg <= RISCV::X31_H))
    return RegStorageType::XReg;
  if ((RISCV::F0_D <= Reg && Reg <= RISCV::F31_D) ||
      (RISCV::F0_F <= Reg && Reg <= RISCV::F31_F) ||
      (RISCV::F0_H <= Reg && Reg <= RISCV::F31_H))
    return RegStorageType::FReg;
  assert(RISCV::V0 <= Reg && Reg <= RISCV::V31 && "unknown register");
  return RegStorageType::VReg;
}

static bool isLegalRVVInstr(unsigned Opcode, const RVVConfiguration &Cfg,
                            unsigned VL, unsigned VLEN,
                            const RISCVSubtarget *ST) {
  if (!isRVV(Opcode))
    return false;
  auto SEW = static_cast<unsigned>(Cfg.SEW);
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
    return isValidEMUL(SEW, EEW, LMUL);
  }
  if (isRVVIndexedLoadStore(Opcode)) {
    // EEW is an index element width.
    auto EEW = getIndexElementWidth(Opcode);
    return isValidEMUL(SEW, EEW, LMUL);
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
    if (!isValidEMUL(SEW, EEW, LMUL))
      return false;
    auto EMUL = computeEMUL(SEW, EEW, LMUL);
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(EMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVIndexedSegLoadStore(Opcode)) {
    auto EEW = getIndexElementWidth(Opcode);
    if (!isValidEMUL(SEW, EEW, LMUL))
      return false;
    auto [Multiplier, IsFractional] = RISCVVType::decodeVLMUL(LMUL);
    if (IsFractional)
      return true;
    return Multiplier * getNumFields(Opcode) <= 8u;
  }
  if (isRVVFloatingPoint(Opcode)) {
    assert(ST);
    if (SEW < 16u &&
        !(mayBeZvfh8BitIntConversion(Opcode) && ST->hasStdExtZvfh()))
      return false;
    if (SEW < 32u && !((isZvfh(Opcode) && ST->hasStdExtZvfh()) ||
                       (isZvfhmin(Opcode) && ST->hasStdExtZvfhmin()))) {
      // If the EEW of a vector floating-point operand does not correspond to a
      // supported IEEE floating-point type, the instruction encoding is
      // reserved.
      return false;
    }
  }
  if (isRVVExt(Opcode)) {
    // If the source EEW is not a supported width, or source EMUL would be below
    // the minimum legal LMUL, the instruction encoding is reserved.
    auto Factor = getRVVExtFactor(Opcode);
    assert(Factor <= SEW);
    assert(SEW % Factor == 0);
    auto EEW = SEW / Factor;
    if (!isLegalSEW(EEW))
      return false;
    return isValidEMUL(SEW, EEW, LMUL);
  }
  if (isRVVIntegerWidening(Opcode) || isRVVFPWidening(Opcode) ||
      isRVVIntegerNarrowing(Opcode) || isRVVFPNarrowing(Opcode)) {
    // Both widening and narrowing instructions use operands with EEW = SEW * 2.
    auto EEW = SEW * 2u;
    if (!isLegalSEW(EEW))
      return false;
    // Check that LMUL * 2 is also legal.
    return isValidEMUL(SEW, EEW, LMUL);
  }
  if (isRVVGather16(Opcode)) {
    // The vrgatherei16.vv form uses SEW/LMUL for the data in vs2 but EEW=16 and
    // EMUL = (16/SEW)*LMUL for the indices in vs1.
    auto EEW = 16u;
    return isValidEMUL(SEW, EEW, LMUL);
  }

  // Instructions from zvbc (carryless multiplication) extension
  // are defined only for SEW = 64.
  if (isZvbc(Opcode) && SEW != 64u)
    return false;
  return true;
}

void takeVSETPrefIntoAccount(VSETWeightOverrides &Overrides) {
  auto Preference = RVVModeChangePreferenceOpt.getValue();
  if (Preference == RVVModeChangeMode::MC_ANY)
    return;

  const auto &Result = Overrides.getEntries();
  auto WeightSum =
      std::accumulate(Result.begin(), Result.end(), 0.0,
                      [](auto Acc, auto Entry) { return Acc + Entry.Weight; });

  switch (Preference) {
  case RVVModeChangeMode::MC_VSETVL:
    Overrides.setVSETVLWeight(WeightSum);
    Overrides.setVSETVLIWeight(0.0);
    Overrides.setVSETIVLIWeight(0.0);
    return;
  case RVVModeChangeMode::MC_VSETVLI:
    Overrides.setVSETVLIWeight(WeightSum);
    Overrides.setVSETVLWeight(0.0);
    Overrides.setVSETIVLIWeight(0.0);
    return;
  case RVVModeChangeMode::MC_VSETIVLI:
    Overrides.setVSETIVLIWeight(WeightSum);
    Overrides.setVSETVLWeight(0.0);
    Overrides.setVSETVLIWeight(0.0);
    return;
  default:
    snippy::fatal("Unknown vset* preference for rvv mode change");
  }
}

RISCVMatInt::InstSeq getIntMatInstrSeq(APInt Value,
                                       InstructionGenerationContext &IGC) {
  [[maybe_unused]] const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  assert((ST.getXLen() == 64 && Value.getBitWidth() == 64) ||
         (ST.getXLen() == 32 && isInt<32>(Value.getSExtValue())));

  auto &ProgCtx = IGC.ProgCtx;
  return RISCVMatInt::generateInstSeq(
      Value.getSExtValue(), ProgCtx.getLLVMState().getSubtargetInfo());
}

// Uses XNOR to reset V0
void generateRVVMaskReset(InstructionGenerationContext &IGC,
                          const MCInstrInfo &InstrInfo,
                          const SnippyTarget &Tgt) {
  if (NoMaskModeForRVV)
    return;

  auto &RGC = IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto &MBB = IGC.MBB;
  auto &Ins = IGC.Ins;
  // It's implied that VL is equal to the width of VM
  auto NewVMWidth = RGC.getActiveRVVMode(MBB).VLVM.VL;

  getSupportInstBuilder(Tgt, MBB, Ins,
                        MBB.getParent()->getFunction().getContext(),
                        InstrInfo.get(RISCV::VMXNOR_MM))
      .addReg(RISCV::V0, RegState::Define)
      .addReg(RISCV::V0, RegState::Undef)
      .addReg(RISCV::V0, RegState::Undef);
  RGC.updateActiveRVVModeVM(&MBB, APInt::getAllOnes(NewVMWidth));
}

// This function creates RVVMode with all ones value in mask and
// default config: LMUL = 1, SEW = GPRegSize, VL = VLEN/SEW
static RVVModeInfo
getSEWXlenVLMaxSupportRVVMode(InstructionGenerationContext &IGC,
                              const MachineBasicBlock &MBB, unsigned VLEN) {
  auto &RGC = IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &VUInfo = RGC.getVUConfigInfo();
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  auto SEW = ST.getELen();

  const auto VL = VLEN / SEW;
  RVVConfigurationInfo::VLVM VLVM{VL, APInt::getMaxValue(VL)};

  const RVVConfiguration *Config;
  if (SEW == 32) {
    Config = &VUInfo.SupportCfgSew32;
  } else if (SEW == 64) {
    Config = &VUInfo.SupportCfgSew64;
  } else {
    llvm_unreachable("Cannot create support RVVMode for this SEW");
  }

  return RVVModeInfo(VLVM, *Config, MBB);
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
                           InstructionGenerationContext &IGC, bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isRVVStridedLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode));

  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  auto &ProgCtx = IGC.ProgCtx;
  auto &TgtCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto VL = TgtCtx.getVL(*MI.getParent());
  const auto &AddrReg = getMemOperand(MI);
  auto AddrRegIdx = MI.getOperandNo(&AddrReg);
  auto AddrValue = AddrInfo.Address;
  if (VL == 0)
    // When VL is zero, we may leave any values in address base and stride
    // registers.
    return std::make_pair(AddressParts{}, MemAddresses{});
  auto &State = ProgCtx.getLLVMState();
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
  auto StrideMultiplier = RandEngine::genInRangeInclusive(-MaxStrideMultiplier,
                                                          MaxStrideMultiplier);
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
                           InstructionGenerationContext &IGC, bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode));

  auto &ProgCtx = IGC.ProgCtx;
  auto &TgtCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
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
  LLVM_DEBUG(dbgs() << "breakDownAddrForRVVIndexed Instruction\n";
             MI.print(dbgs()););
  // From the specification: All Zve* extensions support all vector load and
  // store instructions (Section Section 31.7), except Zve64* extensions do not
  // support EEW=64 for index values when XLEN=32.
  assert(EIEW <= ST.getXLen());
  APInt IndexMaxValue = APInt::getMaxValue(EIEW).zext(ST.getXLen());

  // Start by generating an offset for the base address.
  // The minimum value of the base address's offset is such that after adding
  // the maximum possible offset that can stored in the index register to the
  // final base address, the access is still legal according to the memory
  // scheme
  //
  // We have the following legal range (note: MinOffset <= 0):
  // --|Base + MinOffset ... Base + MaxOffset|--
  // Let Base' be some randomly chosen base address. Let Index' be a random
  // value. in the picked index register. The idea is that Base' + Index' for
  // any Index satisfies the following condition:
  // Base + MinOffset <= Base' + Index' <= Base + MaxOffset
  assert(AddrInfo.MinOffset <= 0);
  assert(AddrInfo.MaxOffset >= 0);

  // Originally we need to create just (xlen + 1)-bit signed immediates, but
  // initial values might be near uint64_t::max (sign bit equals `1`). If xlen
  // is 64-bit, APInt ctor in such cases will create sign extended 65-bit (xlen
  // + 1) immediate that is not the value we need.
  auto Address = APInt(ST.getXLen(), AddrInfo.Address, /* signed */ false)
                     .zext(ST.getXLen() + 1);
  auto MinOffset = APInt(ST.getXLen(), AddrInfo.MinOffset, /* signed */ true)
                       .sext(ST.getXLen() + 1);
  auto MaxOffset = APInt(ST.getXLen(), AddrInfo.MaxOffset, /* signed */ true)
                       .zext(ST.getXLen() + 1);

  // Get the legal range
  bool OV = false;
  auto MinBaseOffset = Address.sadd_ov(MinOffset, OV);
  assert(!OV && "Must not overflow");
  assert(MinBaseOffset.sge(0) && "Cannot be negative");
  MinBaseOffset = MinBaseOffset.trunc(ST.getXLen());
  auto MaxBaseOffset = Address.sadd_ov(MaxOffset, OV).trunc(ST.getXLen());
  assert(!OV && "Must not overflow");

  // MinBaseOffset and MaxBaseOffset must be reachable from AddrInfo.Address
  // with AddrInfo.MinStride
  [[maybe_unused]] uint64_t AddrStrideReminder =
      AddrInfo.Address % AddrInfo.MinStride;
  assert(AddrStrideReminder ==
             (MinBaseOffset.getZExtValue() % AddrInfo.MinStride) &&
         AddrStrideReminder ==
             (MaxBaseOffset.getZExtValue() % AddrInfo.MinStride));

  // Pick the legal random Base' value with the index constraints for current
  // instruction. So, from the equation above:
  // MinBaseOffset <= Base' + IndexMaxValue <= MaxBaseOffset
  // MinBaseOffset - IndexMaxValue  <= Base' <= MaxBaseOffset - IndexMaxValue
  auto MinBase = MinBaseOffset - IndexMaxValue;
  auto MaxBase = MaxBaseOffset - IndexMaxValue;

  // This range can be presented in two types:
  // --|MinBase ... MaxBase|-- and |0 ... MaxBase|--|MinBase ... 2^{xlen} - 1|
  // The second case is valid and can be legal in case of overflow. In such
  // case Index' must wrap Base' around zero

  APInt BaseAddr(ST.getXLen(), 0);

  if (MinBase.ule(MaxBase))
    BaseAddr = RandEngine::genInRangeInclusive(MinBase.getZExtValue(),
                                               MaxBase.getZExtValue());
  else {
    // This is the logic for the case when the valid base address is either
    // in the range from 0 to MaxBase or from MinBase to 2^{xlen} - 1.
    // To ensure a uniform distribution, we first generate a random number
    // within the total length of the valid range.
    //
    // Note: In this case, MaxBase < MinBase, MinBase is included, MaxBase is
    // excluded from the range. AddrRangeLength = (MaxBase - 0) + ((2^{xlen} -
    // 1) - MinBase + 1).

    auto AddrRangeLength =
        APInt::getMaxValue(ST.getXLen()) - MinBase + 1 + MaxBase;
    BaseAddr = RandEngine::genInRangeInclusive(AddrRangeLength);

    // If randomly generated BaseAddr suits to the first range -> nothing to do
    // Otherwise, we pick the value from the second range as
    // BaseAddr = (2^{xlen} - 1) - (BaseAddr - MaxBase),
    // which is equivalent to:
    // BaseAddr = MaxBase - BaseAddr
    if (BaseAddr.ugt(MaxBase))
      BaseAddr = MaxBase - BaseAddr;
  }
  LLVM_DEBUG(dbgs().indent(2)
             << Twine("Generated BaseAddress 0x")
                    .concat(Twine(utohexstr(BaseAddr.getZExtValue())))
                    .concat("\n"));

  auto IndexMinValue = MinBaseOffset - BaseAddr;
  assert(AddrInfo.MinStride != 0);
  auto MaxN = (IndexMaxValue - IndexMinValue).udiv(AddrInfo.MinStride);

  AddressPart MainPart{AddrReg, BaseAddr};

  AddressParts ValueToReg = {MainPart};
  MemAddresses Addresses;
  auto VLEN = TgtCtx.getVLEN();
  unsigned NIdxsPerVReg = VLEN / EIEW;
  while (VL > 0) {
    LLVM_DEBUG(dbgs().indent(2)
               << Twine("Current VL : ").concat(Twine(VL)).concat("\n"));
    // Get the number of indices we can write into one VReg. One VReg can hold
    // no more than NIdxsPerVReg. So, we might need to write not a single
    // register but a group.
    auto NElts = std::min(VL, NIdxsPerVReg);
    auto Offsets = APInt::getZero(VLEN);
    for (size_t ElemIdx = 0; ElemIdx < NElts; ++ElemIdx) {
      // TODO: for unordered stores, generating indices like this is not
      // correct, since store order for overlapping regions is not defined
      auto N = RandEngine::genInRangeInclusive(MaxN);
      auto IndexValue = IndexMinValue + AddrInfo.MinStride * N;

      Offsets.insertBits(IndexValue.getZExtValue(), ElemIdx * EIEW, EIEW);
      LLVM_DEBUG(APInt Index(EIEW, IndexValue.getZExtValue());
                 dbgs().indent(4)
                 << "Generated IndexValue [" + Twine(ElemIdx) + "] : 0x" +
                        Twine(utohexstr(Index.getZExtValue())) + "\n");

      // Verify that generated address is legal according to memory scheme
      [[maybe_unused]] auto FinalAddress = BaseAddr + IndexValue;
      LLVM_DEBUG(dbgs().indent(4)
                 << ("Generated FinalAddress 0x" +
                     Twine(utohexstr(FinalAddress.getZExtValue()))) +
                        "\n");
      assert(FinalAddress.uge(MinBaseOffset));
      assert(FinalAddress.ule(MaxBaseOffset));
      [[maybe_unused]] auto FinalOffset = FinalAddress - AddrInfo.Address;
      assert(FinalOffset.urem(AddrInfo.MinStride) == 0);
      Addresses.push_back((BaseAddr + IndexValue).getZExtValue());
    }
    VL -= NElts;
    ValueToReg.emplace_back(IdxReg++, Offsets);
  }

  return std::make_pair(std::move(ValueToReg), std::move(Addresses));
}

static std::pair<AddressParts, MemAddresses>
breakDownAddrForInstrWithImmOffset(AddressInfo AddrInfo, const MachineInstr &MI,
                                   InstructionGenerationContext &IGC,
                                   bool Is64Bit) {
  auto Opcode = MI.getOpcode();
  assert(isLoadStore(Opcode) || isCLoadStore(Opcode) || isFPLoadStore(Opcode) ||
         isCFPLoadStore(Opcode) || isZicbo(Opcode));

  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &RI = State.getRegInfo();
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
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

using OpcodeFilter = Config::OpcodeFilter;

static OpcodeFilter getRVVDefaultPolicyFilterImpl(const RVVConfiguration &Cfg,
                                                  unsigned VL, unsigned VLEN,
                                                  const RISCVSubtarget *ST) {
  return [&Cfg, VL, VLEN, ST](unsigned Opcode) {
    if (!isRVV(Opcode))
      return true;
    return isLegalRVVInstr(Opcode, Cfg, VL, VLEN, ST);
  };
}

static OpcodeFilter
getDefaultPolicyFilterImpl(const SnippyProgramContext &ProgCtx,
                           const MachineBasicBlock &MBB) {
  auto &State = ProgCtx.getLLVMState();
  auto &RISCVCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  if (!RISCVCtx.hasActiveRVVMode(MBB))
    return [](unsigned Opcode) {
      if (isRVV(Opcode) && !isRVVModeSwitch(Opcode))
        return false;
      return true;
    };

  const auto &Cfg = RISCVCtx.getCurrentRVVCfg(MBB);
  auto VL = RISCVCtx.getVL(MBB);
  auto VLEN = RISCVCtx.getVLEN();

  const auto &ST = State.getSubtarget<RISCVSubtarget>(*MBB.getParent());
  return getRVVDefaultPolicyFilterImpl(Cfg, VL, VLEN, &ST);
}

inline bool checkSupportedOrdering(const OpcodeHistogram &H) {
  if (H.weight(RISCV::LR_W_RL) != 0 || H.weight(RISCV::SC_W_AQ) != 0 ||
      H.weight(RISCV::LR_D_RL) != 0 || H.weight(RISCV::SC_D_AQ) != 0)
    return false;
  return true;
}

static DisableMisalignedAccessMode getMisalignedAccessMode() {
  if (!RISCVDisableMisaligned.isSpecified())
    return DisableMisalignedAccessMode::None;

  return RISCVDisableMisaligned.getValue();
}

template <typename It> static void storeWordToMem(It MemIt, uint32_t Value) {
  auto RegAsBytes = std::vector<uint8_t>{};
  convertNumberToBytesArray(Value, std::back_inserter(RegAsBytes));
  std::copy(RegAsBytes.rbegin(), RegAsBytes.rend(), MemIt);
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
    assert(Inst.begin()->isReg() && "In write instructions, the first operand "
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

class SnippyRISCVTarget final : public SnippyTarget {

  void generateWriteValueSeq(InstructionGenerationContext &IGC, APInt Value,
                             MCRegister DestReg,
                             SmallVectorImpl<MCInst> &Insts) const override {
    if (RISCV::VRRegClass.contains(DestReg))
      snippy::fatal(IGC.ProgCtx.getLLVMState().getCtx(),
                    "Generation register write sequence error",
                    "Writing to register is not implemented for RVV");

    if (RISCV::FPR64RegClass.contains(DestReg) ||
        RISCV::FPR32RegClass.contains(DestReg) ||
        RISCV::FPR16RegClass.contains(DestReg)) {
      generateWriteValueFP(IGC, Value, DestReg, Insts);
      return;
    }
    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
    assert((ST.getXLen() == 64 && Value.getBitWidth() == 64) ||
           (ST.getXLen() == 32 && isInt<32>(Value.getSExtValue())));

    llvm::transform(RISCVMatInt::generateInstSeq(Value.getSExtValue(), ST),
                    std::back_inserter(Insts),
                    [DestReg, SrcReg = Register(RISCV::X0),
                     NumInstr = 0](const auto &Inst) mutable {
                      auto InstBuilder =
                          MCInstBuilder(Inst.getOpcode()).addReg(DestReg);
                      if (NumInstr++ == 1)
                        SrcReg = DestReg;
                      switch (Inst.getOpndKind()) {
                      case RISCVMatInt::Imm:
                        return InstBuilder.addImm(Inst.getImm());
                      case RISCVMatInt::RegX0:
                        return InstBuilder.addReg(SrcReg).addReg(RISCV::X0);
                      case RISCVMatInt::RegReg:
                        return InstBuilder.addReg(SrcReg).addReg(SrcReg);
                      case RISCVMatInt::RegImm:
                        return InstBuilder.addReg(SrcReg).addImm(Inst.getImm());
                      }
                      llvm_unreachable("Unsupported register type");
                    });
  }

public:
  SnippyRISCVTarget() {
    // TODO: use model interface to fetch restricted sections

    // htif
    ReservedRanges.emplace_back(0, 0xFFF1001000, 8, 0xFFF1001000, "rw");
    // clint
    ReservedRanges.emplace_back(0, 0xFFF1000000, 8, 0xFFF1000000, "rwx");
  }

  std::unique_ptr<TargetGenContextInterface>
  createTargetContext(LLVMState &State, const Config &Cfg,
                      const TargetSubtargetInfo *STI) const override;

  std::unique_ptr<TargetConfigInterface> createTargetConfig() const override;

  std::unique_ptr<SimulatorInterface>
  createSimulator(llvm::snippy::DynamicLibrary &ModelLib,
                  const SimulationConfig &Cfg,
                  const TargetGenContextInterface *TgtGenCtx,
                  RVMCallbackHandler *CallbackHandler,
                  const TargetSubtargetInfo &Subtarget) const override;

  const MCRegisterClass &
  getRegClass(InstructionGenerationContext &IGC, unsigned OperandRegClassID,
              unsigned OpIndex, unsigned Opcode,
              const MCRegisterInfo &RegInfo) const override;

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
                            const FeatureBitset &Features) const override {
    return RISCV_MC::isOpcodeAvailable(Opcode, Features) &&
           !snippyRISCVIsOpcodeExcluded(Opcode);
  }
  bool isPseudoAllowed(unsigned Opcode) const override {
    switch (Opcode) {
    case RISCV::PAUSE:
    // Floating-point CSR control.
    case RISCV::ReadFFLAGS:
    // For clearing and reading fflags when used with per-opcode `imm-hist`.
    case RISCV::WriteFFLAGSImm:
    case RISCV::SwapFFLAGSImm:
    case RISCV::ReadFRM:
    // Allows setting rounding mode when combined with per-opcode `imm-hist`.
    case RISCV::WriteFRMImm:
    case RISCV::SwapFRMImm:

    case RISCV::ReadFCSR:
    case RISCV::WriteFCSRImm:
    case RISCV::SwapFCSRImm:
      return true;
    default:
      return false;
    }
  }

  std::unique_ptr<IRegisterState>
  createRegisterState(const TargetSubtargetInfo &ST) const override {
    const auto &RST = static_cast<const RISCVSubtarget &>(ST);
    return std::make_unique<RISCVRegisterState>(RST);
  }

  bool needsGenerationPolicySwitch(unsigned Opcode) const override {
    return isRVVModeSwitch(Opcode);
  }

  std::vector<Register>
  getRegsForSelfcheck(const MachineInstr &MI,
                      InstructionGenerationContext &IGC) const override {
    auto &MBB = IGC.MBB;
    const auto &FirstDestOperand = MI.getOperand(0);
    assert(FirstDestOperand.isReg());

    auto &&SelfcheckSegsInfo = getInfoAboutRegsForSelfcheck(
        MI.getOpcode(), FirstDestOperand.getReg(), MBB, IGC.ProgCtx);

    std::vector<Register> Regs;
    for (const auto &SelfcheckRegInfo : SelfcheckSegsInfo) {
      std::vector<unsigned> RegIdxs(SelfcheckRegInfo.NumRegs);
      std::iota(RegIdxs.begin(), RegIdxs.end(),
                SelfcheckRegInfo.BaseDestRegister.id());
      std::copy(RegIdxs.begin(), RegIdxs.end(), std::back_inserter(Regs));
    }
    return Regs;
  }

  std::vector<OpcodeHistogramEntry>
  getPolicyOverrides(const SnippyProgramContext &ProgCtx,
                     const MachineBasicBlock &MBB) const override {
    auto &RISCVCtx =
        ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    auto Overrides = RISCVCtx.getVSETOverrides(MBB);
    takeVSETPrefIntoAccount(Overrides);
    auto Entries = Overrides.getEntries();
    return {Entries.begin(), Entries.end()};
  }

  bool groupMustHavePrimaryInstr(const SnippyProgramContext &ProgCtx,
                                 const MachineBasicBlock &MBB) const override {
    auto &RISCVCtx =
        ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    return RISCVCtx.hasActiveRVVMode(MBB);
  }

  Config::OpcodeFilter
  getDefaultPolicyFilter(const SnippyProgramContext &ProgCtx,
                         const MachineBasicBlock &MBB) const override {
    return getDefaultPolicyFilterImpl(ProgCtx, MBB);
  }

  void checkInstrTargetDependency(const OpcodeHistogram &H) const override {
    if (!checkSupportedOrdering(H))
      snippy::fatal("Lr.rl and Sc.aq are prohibited by RISCV ISA");
  }

  void generateRegsInit(InstructionGenerationContext &IGC,
                        const IRegisterState &R) const override {
    const auto &Regs = static_cast<const RISCVRegisterState &>(R);

    // Done before GPR initialization since scratch registers are used
    if (!Regs.FRegs.empty())
      generateFPRInit(IGC, Regs);

    // Done before GPR initialization since scratch registers are used
    if (!Regs.VRegs.empty() &&
        (RVVInitModeOpt.getValue() == RVVInitMode::Mixed ||
         RVVInitModeOpt.getValue() == RVVInitMode::Slides ||
         RVVInitModeOpt.getValue() == RVVInitMode::Loads))
      generateVRegsInit(IGC, Regs);

    generateGPRInit(IGC, Regs);

    if (!Regs.VRegs.empty() && RVVInitModeOpt.getValue() == RVVInitMode::Splats)
      generateVRegsInitWithSplats(IGC, Regs);
  }

  unsigned getFPRegsCount(const TargetSubtargetInfo &ST) const override {
    const auto &RST = static_cast<const RISCVSubtarget &>(ST);
    unsigned RegsCount = 0;
    if (RST.hasStdExtZfh() || RST.hasStdExtZfhmin())
      RegsCount += RISCV::FPR16RegClass.getNumRegs();
    if (RST.hasStdExtF())
      RegsCount += RISCV::FPR32RegClass.getNumRegs();
    if (RST.hasStdExtD())
      RegsCount += RISCV::FPR64RegClass.getNumRegs();

    return RegsCount;
  }

  void generateGPRInit(InstructionGenerationContext &IGC,
                       const RISCVRegisterState &Regs) const {
    auto &MBB = IGC.MBB;
    auto RP = IGC.pushRegPool();
    // Initialize registers (except X0) before taking a branch
    assert(Regs.XRegs[0] == 0);
    for (auto [RegIdx, Value] : drop_begin(enumerate(Regs.XRegs))) {
      auto Reg = regIndexToMCReg(IGC, RegIdx, RegStorageType::XReg);
      if (!RP->isReserved(Reg, MBB))
        writeValueToReg(IGC, APInt(getRegBitWidth(Reg, IGC), Value), Reg);
    }
  }

  void generateFPRInit(InstructionGenerationContext &IGC,
                       const RISCVRegisterState &Regs) const {
    auto &MBB = IGC.MBB;
    auto RP = IGC.pushRegPool();
    // Initialize registers before taking a branch
    for (auto [RegIdx, Value] : enumerate(Regs.FRegs)) {
      auto FPReg = regIndexToMCReg(IGC, RegIdx, RegStorageType::FReg);
      if (!RP->isReserved(FPReg, MBB))
        writeValueToReg(IGC, APInt(getRegBitWidth(FPReg, IGC), Value), FPReg);
    }
  }

  void generateVRegsInit(InstructionGenerationContext &IGC,
                         const RISCVRegisterState &Regs) const {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &State = ProgCtx.getLLVMState();
    auto RP = IGC.pushRegPool();

    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
    const auto &InstrInfo = State.getInstrInfo();

    assert(ST.hasStdExtV());

    // V0 to init before anything vector-related
    if (!NoMaskModeForRVV) {
      auto InitV0 = Regs.VRegs[0];
      writeValueToReg(IGC, InitV0, RISCV::V0);
    }
    rvvGenerateModeSwitchAndUpdateContext(InstrInfo, IGC);

    generateNonMaskVRegsInit(IGC, Regs, [](Register Reg) { return false; });
  }

  // If Filter(Reg) is true, than Reg won't be inited
  template <typename T>
  void generateNonMaskVRegsInit(InstructionGenerationContext &IGC,
                                const RISCVRegisterState &Regs,
                                const T &Filter) const {
    auto &MBB = IGC.MBB;
    auto &RP = IGC.getRegPool();
    // V0 is the mask register, skip it
    for (auto [RegIdx, Value] : drop_begin(enumerate(Regs.VRegs))) {
      auto Reg = regIndexToMCReg(IGC, RegIdx, RegStorageType::VReg);
      // Skip reserved registers
      if (RP.isReserved(Reg, MBB) || Filter(Reg))
        continue;
      writeValueToReg(IGC, Value, Reg);
    }
  }

  void generateVRegsInitWithSplats(InstructionGenerationContext &IGC,
                                   const RISCVRegisterState &Regs) const {
    auto &MBB = IGC.MBB;
    auto &ProgCtx = IGC.ProgCtx;
    const auto &State = ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
    auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    auto RP = IGC.pushRegPool();

    assert(ST.hasStdExtV());

    const auto VLEN = RGC.getVLEN();
    const auto &NewRVVMode = getSEWXlenVLMaxSupportRVVMode(IGC, MBB, VLEN);
    generateRVVModeUpdate(IGC, InstrInfo, NewRVVMode);

    auto InsertPos = MBB.getFirstTerminator();
    // Initialize registers before taking a branch
    // V0 is the mask register, skip it.
    for (unsigned RegNo = 1; RegNo < Regs.XRegs.size(); ++RegNo) {
      auto XReg = regIndexToMCReg(IGC, RegNo, RegStorageType::XReg);
      auto VReg = regIndexToMCReg(IGC, RegNo, RegStorageType::VReg);
      if (!RP->isReserved(VReg, MBB))
        getSupportInstBuilder(*this, MBB, InsertPos,
                              MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(RISCV::VMV_V_X), VReg)
            .addReg(XReg);
    }
  }

  bool is64Bit(const TargetMachine &TM) const override {
    assert(TM.getTargetTriple().isRISCV());
    return TM.getTargetTriple().isRISCV64();
  }

  bool isMultipleReg(Register Reg, const MCRegisterInfo &RI) const override {
    if (Reg == RISCV::NoRegister)
      return false;
    // If there is only one subreg in subregs,
    // then this register does not consist of smaller ones, which means it is
    // physical
    auto Subregs = RI.subregs_inclusive(Reg);
    return std::distance(Subregs.begin(), Subregs.end()) != 1;
  }

  bool isPhysRegClass(unsigned RegClassID,
                      const MCRegisterInfo &RI) const override {
    if (RegClassID == RISCV::VMV0RegClassID)
      return false;
    const auto &RC = RI.getRegClass(RegClassID);
    return std::all_of(RC.begin(), RC.end(), [this, &RI](unsigned Reg) {
      return !isMultipleReg(Reg, RI);
    });
  }

  Register getFirstPhysReg(Register Reg,
                           const MCRegisterInfo &RI) const override {
    if (Reg == RISCV::NoRegister)
      return Reg;
    auto Subregs = RI.subregs_inclusive(Reg);
    // The following comparisons rely on the location of the registers in the
    // file RISCVGenRegisterInfo.inc. All registers except FP are written in
    // order of increasing size.
    static_assert(
        RISCV::F0_D < RISCV::F0_F && RISCV::F0_F < RISCV::F0_H &&
        "The value of enum is expected to decrease with increasing size of "
        "the FP register");
    if (Reg >= RISCV::F0_D && Reg <= RISCV::F31_H)
      return *std::max_element(Subregs.begin(), Subregs.end());
    // Select the smallest of the subregisters, which is in fact a physical
    // register.
    static_assert(
        RISCV::V0 < RISCV::V0M2 &&
        "The value of enum is expected to increase with increasing size of "
        "the rvv register");
    static_assert(
        RISCV::X0 < RISCV::X31 &&
        "The value of enum is expected to increase with increasing size of "
        "the GPR register");
    return *std::min_element(Subregs.begin(), Subregs.end());
  }

  void
  getPhysRegsFromUnit(Register RegUnit, const MCRegisterInfo &RI,
                      SmallVectorImpl<Register> &OutPhysRegs) const override {
    OutPhysRegs.clear();
    if (RegUnit == RISCV::NoRegister)
      return;
    if (!isMultipleReg(RegUnit, RI)) {
      OutPhysRegs.push_back(RegUnit);
      return;
    }

    auto Subregs = RI.subregs_inclusive(RegUnit);
    copy_if(Subregs, std::back_inserter(OutPhysRegs),
            [this, &RI](auto &SubReg) { return !isMultipleReg(SubReg, RI); });
  }

  // This function is different from getPhysRegsFromUnit in that
  // it returns physical registers without overlaps.
  //
  // E.G.:
  //         getPhysRegsWithoutOverlaps(F0_D) -> F0_D
  //         getPhysRegsFromUnit(F0_D)        -> F0_D, F0_F, F0_H
  void getPhysRegsWithoutOverlaps(
      Register RegUnit, const MCRegisterInfo &RI,
      SmallVectorImpl<Register> &OutPhysRegs) const override {
    static_assert(
        RISCV::F0_D < RISCV::F31_D && RISCV::F0_F < RISCV::F31_F &&
        RISCV::F0_H < RISCV::F31_H &&
        "The value of enum is expected to increase with increasing number of "
        "FP register");
    OutPhysRegs.clear();
    if ((RISCV::F0_D <= RegUnit && RegUnit <= RISCV::F31_D) ||
        (RISCV::F0_F <= RegUnit && RegUnit <= RISCV::F31_F) ||
        (RISCV::F0_H <= RegUnit && RegUnit <= RISCV::F31_H)) {
      OutPhysRegs.push_back(RegUnit);
      return;
    }
    if ((RISCV::X0 <= RegUnit && RegUnit <= RISCV::X31) ||
        (RISCV::X0_W <= RegUnit && RegUnit <= RISCV::X31_W) ||
        (RISCV::X0_H <= RegUnit && RegUnit <= RISCV::X31_H)) {
      OutPhysRegs.push_back(RegUnit);
      return;
    }
    getPhysRegsFromUnit(RegUnit, RI, OutPhysRegs);
  }

  bool isSelfcheckAllowed(unsigned Opcode) const override {
    if (isRVV(Opcode) && !SelfcheckRVV) {
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
      snippy::fatal("This memory instruction unsupported");

    // FIXME: here explicitly placed all vector istructions that use V0 mask
    // explicitly and this can not be changed
    if (NoMaskModeForRVV &&
        (isRVVuseV0RegExplicitly(Opcode) || isRVVuseV0RegImplicitly(Opcode))) {
      snippy::fatal("In histogram given a vector opcode with explicit V0 "
                    "mask usage, but snippy was given option that forbids "
                    "any masks for vector instructions");
    }
    return false;
  }

  void getEncodedMCInstr(const MachineInstr *MI, const MCCodeEmitter &MCCE,
                         AsmPrinter &AP, const MCSubtargetInfo &STI,
                         SmallVector<char> &OutBuf) const override {
    MCInst OutInst;
    auto &RVAP = static_cast<RISCVAsmPrinter &>(AP);
    if (!RVAP.lowerPseudoInstExpansion(MI, OutInst))
      RVAP.lowerToMCInst(MI, OutInst);

    SmallVector<MCFixup> Fixups;
    MCCE.encodeInstruction(OutInst, OutBuf, Fixups, STI);
  }

  void generateCustomInst(
      const MCInstrDesc &InstrDesc,
      planning::InstructionGenerationContext &InstrGenCtx) const override {
    assert(requiresCustomGeneration(InstrDesc));
    auto Opcode = InstrDesc.getOpcode();
    assert(isRVVModeSwitch(Opcode));
    auto &ProgCtx = InstrGenCtx.ProgCtx;
    rvvGenerateModeSwitchAndUpdateContext(ProgCtx.getLLVMState().getInstrInfo(),
                                          InstrGenCtx, Opcode);
  }

  void instructionPostProcess(InstructionGenerationContext &IGC,
                              MachineInstr &MI) const override;
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
  static constexpr unsigned kCompressedInstrSize = 2;

  unsigned getMaxInstrSize() const override { return kMaxInstrSize; }

  std::set<unsigned>
  getPossibleInstrsSize(const TargetSubtargetInfo &STI) const override {
    const auto &ST = static_cast<const RISCVSubtarget &>(STI);
    bool STSupportsCompressed = ST.hasStdExtC() || ST.hasStdExtZca() ||
                                ST.hasStdExtZcb() || ST.hasStdExtZcd() ||
                                ST.hasStdExtZce() || ST.hasStdExtZcf();
    if (STSupportsCompressed)
      return {kCompressedInstrSize, kMaxInstrSize};
    return {kMaxInstrSize};
  }

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

  MachineBasicBlock *
  generateBranch(InstructionGenerationContext &IGC,
                 const MCInstrDesc &InstrDesc) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto RP = IGC.pushRegPool();
    auto &MBB = IGC.MBB;
    auto *MF = MBB.getParent();
    auto *NextMBB = createMachineBasicBlock(*MF);
    MF->insert(++MachineFunction::iterator(&MBB), NextMBB);
    NextMBB->transferSuccessorsAndUpdatePHIs(&MBB);
    MBB.addSuccessor(NextMBB);

    auto Opcode = InstrDesc.getOpcode();
    const auto &InstrInfo = State.getInstrInfo();
    const auto &BranchDesc = InstrInfo.get(Opcode);
    if (BranchDesc.isUnconditionalBranch()) {
      const auto *RVInstrInfo =
          State.getSubtarget<RISCVSubtarget>(*MF).getInstrInfo();
      RVInstrInfo->insertUnconditionalBranch(MBB, NextMBB, DebugLoc());
      return NextMBB;
    }

    const auto &RegInfo = State.getRegInfo();
    auto MIB = getMainInstBuilder(*this, MBB, MBB.end(),
                                  MBB.getParent()->getFunction().getContext(),
                                  BranchDesc);
    const auto &MCRegClass =
        RegInfo.getRegClass(BranchDesc.operands()[0].RegClass);
    auto FirstReg = RP->getAvailableRegister("for branch condition", RegInfo,
                                             MCRegClass, MBB);
    MIB.addReg(FirstReg);
    if (!isCompressedBranch(Opcode)) {
      unsigned SecondReg = RP->getAvailableRegister("for branch condition",
                                                    RegInfo, MCRegClass, MBB);
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
                                      SnippyProgramContext &ProgCtx) const {
    auto Opcode = Branch.getOpcode();
    assert(isCompressedBranch(Opcode) && "Compressed branch expected");
    auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto UncompOpcode = Opcode == RISCV::C_BEQZ ? RISCV::BEQ : RISCV::BNE;
    auto *MBB = Branch.getParent();
    assert(MBB);
    auto CondOp = Branch.getOperand(0);
    assert(CondOp.isReg());
    auto CondReg = CondOp.getReg();
    auto *DstMBB = getBranchDestination(Branch);
    assert(DstMBB);
    auto &MI = *getMainInstBuilder(*this, *MBB, Branch,
                                   MBB->getParent()->getFunction().getContext(),
                                   InstrInfo.get(UncompOpcode))
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
                              SnippyProgramContext &ProgCtx) const {
    auto *ProcessedBranch = &Branch;
    auto *MBB = Branch.getParent();
    assert(MBB);

    // We need to uncompress branch because all actions below don't expect
    // compressed branch
    if (isCompressedBranch(Branch.getOpcode()))
      ProcessedBranch = relaxCompressedBranch(Branch, ProgCtx);
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
    assert(checkMetadata(*FallbackBR, SnippyMetadata::Support));
    ProcessedBranch->eraseFromParent();
    FallbackBR->eraseFromParent();
    RVInstrInfo.insertBranch(*MBB, FBB, TBB, Cond, DebugLoc());
    setAsSupportInstr(MBB->back(), ProgCtx.getLLVMState().getCtx());
    return &*MBB->getFirstTerminator();
  }

  bool relaxBranch(MachineInstr &Branch, unsigned Distance,
                   SnippyProgramContext &ProgCtx) const override {
    assert(Branch.isBranch());
    if (fitsCompressedBranch(Distance))
      return true;
    if (fitsBranch(Distance) && isCompressedBranch(Branch.getOpcode()))
      return relaxCompressedBranch(Branch, ProgCtx) != nullptr;
    if (fitsJump(Distance))
      return relaxWithJump(Branch, ProgCtx) != nullptr;

    return false;
  }

  void insertFallbackBranch(MachineBasicBlock &From, MachineBasicBlock &To,
                            const LLVMState &State) const override {
    const auto &InstrInfo = State.getInstrInfo();
    getSupportInstBuilder(*this, From, From.end(),
                          From.getParent()->getFunction().getContext(),
                          InstrInfo.get(RISCV::PseudoBR))
        .addMBB(&To);
  }

  bool replaceBranchDest(MachineInstr &Branch,
                         MachineBasicBlock &NewDestMBB) const override {
    auto *OldDestBB = getBranchDestination(Branch);
    if (OldDestBB == &NewDestMBB)
      return false;
    assert(Branch.getNumExplicitOperands() >= 1);
    auto DestBBOpNum = Branch.getNumExplicitOperands() - 1;
    Branch.removeOperand(DestBBOpNum);

    auto NewDestOperand = MachineOperand::CreateMBB(&NewDestMBB);
    Branch.addOperand(NewDestOperand);

    auto *BranchBB = Branch.getParent();
    assert(BranchBB);
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

  /// If target supports compressed instructions return GPRC, use GPR either
  const MCRegisterClass &
  getMCRegClassForBranch(SnippyProgramContext &ProgCtx,
                         const MachineInstr &Instr) const override {
    assert(Instr.isBranch() && "Branch expected");
    auto OpsInfo = Instr.getDesc().operands();
    auto *RegOperand =
        std::find_if(OpsInfo.begin(), OpsInfo.end(), [](const auto &OpInfo) {
          return OpInfo.OperandType == MCOI::OperandType::OPERAND_REGISTER;
        });
    assert(RegOperand != OpsInfo.end() &&
           "All supported branches expected to have at least one register "
           "operand");
    auto &State = ProgCtx.getLLVMState();
    auto &RI = State.getRegInfo();
    auto RCID = RegOperand->RegClass;
    if (RCID == RISCV::GPRCRegClassID ||
        !State.getSubtarget<RISCVSubtarget>(*Instr.getParent()->getParent())
             .hasStdExtC())
      return RI.getRegClass(RCID);

    auto CompressionMode = LoopControlLogicCompression.getValue();
    if (CompressionMode == LoopControlLogicCompressionMode::Off)
      return RI.getRegClass(RISCV::GPRNoGPRCRegClassID);

    constexpr auto MinNumOfBranchGPRC = 2;
    auto *MBB = Instr.getParent();
    assert(MBB);
    auto RP = ProgCtx.getRegisterPool();
    auto NAvailableRegs =
        RP.getNumAvailable(RI.getRegClass(RISCV::GPRCRegClassID), *MBB);
    if (CompressionMode == LoopControlLogicCompressionMode::On &&
        NAvailableRegs >= MinNumOfBranchGPRC)
      return RI.getRegClass(RISCV::GPRCRegClassID);

    return RI.getRegClass(RISCV::GPRRegClassID);
  }

  /// RISCV Loops:
  ///
  /// If loop-counters: random-init is set, we have an Offset for
  /// register values. Only zero Offset is supported for C_BNEZ.
  /// If loop-counters: random-stride is set, we have a StrideOffset for
  /// register values.
  /// * BEQ, C_BEQZ:
  ///   -- Init --
  ///     Stride = 1 (= StrideOffset)
  ///     CounterReg = 0 (Offset)
  ///   -- Latch --
  ///     CounterReg += Stride
  ///     LimitReg = Offset + StrideOffset * NIter - 1
  ///     LimitReg = SLT LimitReg, CounterReg
  ///   -- Branch --
  ///     BEQ LimitReg, X0
  ///     C_BEQZ LimitReg
  ///
  /// * BNE, C_BNEZ:
  ///   -- Init --
  ///     Stride = 1 (= StrideOffset)
  ///     LimitReg = 0 (= Offset)
  ///     CounterReg = NIter (= NIter * Stride + Offset)
  ///   -- Latch --
  ///     CounterReg -= Stride
  ///   -- Branch --
  ///     BNE CounterReg, LimitReg
  ///     C_BNEZ CounterReg
  ///
  /// * BLT, BLTU:
  ///   -- Init --
  ///     Stride = 1 (= StrideOffset)
  ///     LimitReg = NIter (= NIter * Stride + Offset)
  ///     CounterReg = 0 (= Offset)
  ///   -- Latch --
  ///     CounterReg += Stride
  ///   -- Branch --
  ///     BLT CounterReg, LimitReg
  ///     BLTU CounterReg, LimitReg
  ///
  /// * BGE, BGEU:
  ///   -- Init --
  ///     Stride = 1 (= StrideOffset)
  ///     LimitReg = 0 (= Offset)
  ///     CounterReg = NIter (= NIter * Stride + Offset)
  ///   -- Latch --
  ///     CounterReg -= Stride
  ///   -- Branch --
  ///     BGE CounterReg, LimitReg
  ///     BGEU CounterReg, LimitReg

  MachineInstr &
  updateLoopBranch(MachineInstr &Branch, const MCInstrDesc &InstrDesc,
                   ArrayRef<Register> ReservedRegs) const override {
    assert(Branch.isBranch() && "Branch expected");
    auto *BranchMBB = Branch.getParent();
    auto *DestBB = getBranchDestination(Branch);
    auto Opcode = Branch.getOpcode();
    bool EqBranch = isEqBranch(Opcode);
    assert(!EqBranch ||
           ReservedRegs.size() == MaxNumOfReservedRegsForLoop &&
               "Equal branches expected to have two reserved registers");

    auto FirstReg =
        EqBranch ? ReservedRegs[LimitRegIdx] : ReservedRegs[CounterRegIdx];
    auto NewBranch =
        getMainInstBuilder(*this, *BranchMBB, Branch,
                           BranchMBB->getParent()->getFunction().getContext(),
                           InstrDesc)
            .addReg(FirstReg);
    if (!isCompressedBranch(Opcode)) {
      auto SecondReg = EqBranch ? RISCV::X0 : ReservedRegs[LimitRegIdx];
      NewBranch.addReg(SecondReg);
    }
    NewBranch.addMBB(DestBB);

    return *NewBranch.getInstr();
  }

  unsigned
  getNumRegsForLoopBranch(const MCInstrDesc &BranchDesc) const override {
    assert(BranchDesc.isBranch() && "Branch expected");
    auto Opcode = BranchDesc.getOpcode();
    bool EqBranch = isEqBranch(Opcode);

    auto FilterOpReg = [](const auto &OpInfo) {
      return OpInfo.OperandType == MCOI::OperandType::OPERAND_REGISTER;
    };
    unsigned NumRegsToReserv =
        EqBranch ? MaxNumOfReservedRegsForLoop
                 : count_if(BranchDesc.operands(), FilterOpReg);

    assert((NumRegsToReserv >= MinNumOfReservedRegsForLoop) &&
           (NumRegsToReserv <= MaxNumOfReservedRegsForLoop) &&
           "Only branches with one or two register operands are expected for "
           "RISC-V");

    return NumRegsToReserv;
  }

  unsigned getInstrSize(const MachineInstr &Inst,
                        LLVMState &State) const override {
    auto &RISCVSTI =
        State.getSubtarget<RISCVSubtarget>(*Inst.getParent()->getParent());
    auto *RISCVII = RISCVSTI.getInstrInfo();
    assert(RISCVII);
    return RISCVII->getInstSizeInBytes(Inst);
  }

  LoopType getLoopType(MachineInstr &Branch) const override {
    switch (Branch.getOpcode()) {
    case RISCV::BEQ:
    case RISCV::C_BEQZ:
    case RISCV::BLT:
    case RISCV::BLTU:
      return LoopType::UpCount;
    case RISCV::BNE:
    case RISCV::C_BNEZ:
    case RISCV::BGE:
    case RISCV::BGEU:
      return LoopType::DownCount;
    default:
      llvm_unreachable("Unsupported branch type");
    }
  }

  static unsigned getMaxGenValueForRegs(InstructionGenerationContext &IGC,
                                        const Branchegram &Branches,
                                        ArrayRef<Register> ReservedRegs) {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
    auto VLEN =
        ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>().getVLEN();
    auto CounterReg = ReservedRegs[CounterRegIdx];
    unsigned MaxCounterRegVal = RISCVRegisterState::getMaxRegValueForSize(
        CounterReg, ST.getXLen(), VLEN);
    if (ReservedRegs.size() == 1)
      return MaxCounterRegVal;

    auto LimitReg = ReservedRegs[LimitRegIdx];
    unsigned MaxLimitRegVal =
        RISCVRegisterState::getMaxRegValueForSize(LimitReg, ST.getXLen(), VLEN);
    return std::min(MaxCounterRegVal, MaxLimitRegVal);
  }

  using WarnDataPair = std::pair<std::optional<SnippyDiagnosticInfo>, unsigned>;

  static WarnDataPair
  getRandLoopCounterInitValue(InstructionGenerationContext &IGC, unsigned NIter,
                              const Branchegram &Branches,
                              ArrayRef<Register> ReservedRegs,
                              unsigned MaxCounterVal) {
    if (!Branches.isRandomCountersInitRequested())
      return {std::nullopt, 0u};

    auto [MinRegOpt, MaxRegOpt] = Branches.LoopCounters.InitRange.value();
    auto Min = MinRegOpt.value_or(0);
    // We need to handle the case when our counter may overflows during the
    // iteration.
    auto BoundaryVal = MaxCounterVal - NIter;
    auto UnsafeMax = MaxRegOpt.value_or(MaxCounterVal);
    auto Max = std::min(UnsafeMax, BoundaryVal);
    if (Max < Min)
      return {
          SnippyDiagnosticInfo(
              "The range of values for initializing loop "
              "counters conflicts with the number of iterations (overflow is "
              "possible)",
              "The loop counters will be initialized with the closest possible "
              "number to the specified minimum value of the range.",
              llvm::DS_Warning, WarningName::LoopCounterOutOfRange),
          Max};
    return {std::nullopt, RandEngine::genInRangeInclusive(Min, Max)};
  }

  static WarnDataPair getRandLoopCounterStrideValue(
      InstructionGenerationContext &IGC, const Branchegram &Branches,
      ArrayRef<Register> ReservedRegs, unsigned RegRandOffset, unsigned NIter,
      unsigned MaxCounterVal) {
    if (!Branches.isRandomCountersStrideRequested())
      return {std::nullopt, 1u};

    auto IncOpcode = getIncOpcodeForLoopCounter(IGC);
    unsigned MaxImmVal = maxIntN(getImmSizeInBits(IncOpcode));
    auto [MinStrideOpt, MaxStrideOpt] =
        Branches.LoopCounters.StrideRange.value();
    unsigned Min = std::min(MinStrideOpt.value_or(1), MaxImmVal);
    unsigned Max = std::min(MaxStrideOpt.value_or(MaxImmVal), MaxImmVal);
    // We need to handle the case where the specified range is outside the valid
    // range for the immediate of a loop counter increment instruction. We also
    // need to handle the case when our counter may overflows during the
    // iteration.
    auto BoundaryVal = MaxCounterVal - RegRandOffset;
    auto IsOverflowPossible = Max * NIter > BoundaryVal;
    if ((MinStrideOpt.has_value() && MinStrideOpt.value() > MaxImmVal) ||
        (MaxStrideOpt.has_value() && MaxStrideOpt.value() > MaxImmVal) ||
        IsOverflowPossible) {
      if (IsOverflowPossible)
        Max = std::max(1u, BoundaryVal / NIter);
      if (Max < Min)
        Min = 1;
      return {SnippyDiagnosticInfo(
                  llvm::formatv(
                      "The stride range is now limited to '[{0}, {1}]' to "
                      "prevent overflow errors. Try reducing one of the "
                      "following: the value "
                      "range for loop counter initialization, the value range "
                      "for the stride, or the number of iterations.",
                      Min, Max),
                  llvm::DS_Warning, WarningName::LoopStrideOutOfRange),
              RandEngine::genInRangeInclusive(Min, Max)};
    }
    assert(Max >= Min);
    return {std::nullopt, RandEngine::genInRangeInclusive(Min, Max)};
  }

  LoopCounterInitResult insertLoopInit(InstructionGenerationContext &IGC,
                                       MachineInstr &Branch,
                                       const Branchegram &Branches,
                                       ArrayRef<Register> ReservedRegs,
                                       unsigned NIter) const override {
    assert(Branch.isBranch() && "Branch expected");
    assert((ReservedRegs.size() != MaxNumOfReservedRegsForLoop) ||
           (ReservedRegs[CounterRegIdx] != ReservedRegs[LimitRegIdx]) &&
               "Counter and Limit registers expected to be different");

    auto CounterReg = ReservedRegs[CounterRegIdx];
    auto MaxCounterVal = getMaxGenValueForRegs(IGC, Branches, ReservedRegs);
    auto [InitDiag, RegRandOffset] = getRandLoopCounterInitValue(
        IGC, NIter, Branches, ReservedRegs, MaxCounterVal);
    auto [StrideDiag, RegRandStride] = getRandLoopCounterStrideValue(
        IGC, Branches, ReservedRegs, RegRandOffset, NIter, MaxCounterVal);
    switch (Branch.getOpcode()) {
    case RISCV::BEQ:
    case RISCV::C_BEQZ: {
      auto StartCounterRegVal = RegRandOffset;
      writeValueToReg(
          IGC, APInt(getRegBitWidth(CounterReg, IGC), StartCounterRegVal),
          CounterReg);
      break;
    }
    case RISCV::BNE: {
      auto StartCounterRegVal = NIter * RegRandStride + RegRandOffset;
      writeValueToReg(
          IGC, APInt(getRegBitWidth(CounterReg, IGC), StartCounterRegVal),
          CounterReg);
      auto LimitReg = ReservedRegs[LimitRegIdx];
      auto LimitRegVal = RegRandOffset;
      writeValueToReg(IGC, APInt(getRegBitWidth(LimitReg, IGC), LimitRegVal),
                      LimitReg);
      break;
    }
    case RISCV::C_BNEZ: {
      auto StartCounterRegVal = NIter * RegRandStride;
      writeValueToReg(
          IGC, APInt(getRegBitWidth(CounterReg, IGC), StartCounterRegVal),
          CounterReg);
      assert(
          (ReservedRegs.size() == MinNumOfReservedRegsForLoop) &&
          "In RISC-V for compressed branch C_BNEZ only one register CounterReg "
          "expected to be reserved for loop");
      RegRandOffset = 0;
      break;
    }
    case RISCV::BLT:
    case RISCV::BLTU: {
      auto StartCounterRegVal = RegRandOffset;
      writeValueToReg(
          IGC, APInt(getRegBitWidth(CounterReg, IGC), StartCounterRegVal),
          CounterReg);
      auto LimitReg = ReservedRegs[LimitRegIdx];
      auto LimitRegVal = NIter * RegRandStride + RegRandOffset;
      writeValueToReg(IGC, APInt(getRegBitWidth(LimitReg, IGC), LimitRegVal),
                      LimitReg);
      break;
    }
    case RISCV::BGE:
    case RISCV::BGEU: {
      auto StartCounterRegVal = NIter * RegRandStride + RegRandOffset;
      writeValueToReg(
          IGC, APInt(getRegBitWidth(CounterReg, IGC), StartCounterRegVal),
          CounterReg);
      auto LimitReg = ReservedRegs[LimitRegIdx];
      auto LimitRegVal = RegRandOffset + 1;
      writeValueToReg(IGC, APInt(getRegBitWidth(LimitReg, IGC), LimitRegVal),
                      LimitReg);
      break;
    }
    default:
      llvm_unreachable("Unsupported branch type");
    }
    return {InitDiag, StrideDiag,
            APInt(getRegBitWidth(CounterReg, IGC), RegRandOffset),
            APInt(getRegBitWidth(CounterReg, IGC), RegRandStride)};
  }

  LoopCounterInsertionResult insertLoopCounter(
      InstructionGenerationContext &IGC, MachineInstr &Branch,
      ArrayRef<Register> ReservedRegs, unsigned NIter,
      RegToValueType &ExitingValues,
      const LoopCounterInitResult &CounterInitInfo) const override {
    assert(Branch.isBranch() && "Branch expected");
    assert(ReservedRegs.size() != MaxNumOfReservedRegsForLoop ||
           ReservedRegs[CounterRegIdx] != ReservedRegs[LimitRegIdx] &&
               "Counter and limit registers expected to be different");
    assert(NIter);

    auto Pos = IGC.Ins;
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &MBB = *Pos->getParent();
    const auto &InstrInfo = State.getInstrInfo();
    APInt MinCounterVal;
    auto ADDIOp = getIncOpcodeForLoopCounter(IGC);

    auto RegCounterOffset = CounterInitInfo.MinCounterVal.getZExtValue();
    auto CounterReg = ReservedRegs[CounterRegIdx];

    switch (Branch.getOpcode()) {
    case RISCV::BEQ:
    case RISCV::C_BEQZ: {
      auto LimitReg = ReservedRegs[LimitRegIdx];
      getSupportInstBuilder(*this, MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(ADDIOp))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(CounterInitInfo.StrideVal.getZExtValue());
      // Since we have only two reserved registers for the loop, we have to
      // update the boundary value for the CounterReg at each iteration, which
      // we write into the LimitReg.
      writeValueToReg(
          IGC,
          APInt(getRegBitWidth(CounterReg, IGC),
                RegCounterOffset +
                    CounterInitInfo.StrideVal.getZExtValue() * NIter - 1),
          LimitReg);

      getSupportInstBuilder(*this, MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(RISCV::SLT))
          .addReg(LimitReg, RegState::Define)
          .addReg(LimitReg)
          .addReg(CounterReg);
      ExitingValues[CounterReg] = APInt(
          getRegBitWidth(CounterReg, IGC),
          RegCounterOffset + CounterInitInfo.StrideVal.getZExtValue() * NIter);
      MinCounterVal =
          APInt(getRegBitWidth(LimitReg, IGC), RegCounterOffset + 1);
      ExitingValues[LimitReg] = APInt(getRegBitWidth(LimitReg, IGC), 1);
      break;
    }
    case RISCV::C_BNEZ:
      assert(RegCounterOffset == 0 &&
             "C_BNEZ is not supported with non zero value of the loop counter");
      [[fallthrough]];
    case RISCV::BNE:
      getSupportInstBuilder(*this, MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(ADDIOp))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(-CounterInitInfo.StrideVal.getSExtValue());
      ExitingValues[CounterReg] =
          APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset);
      MinCounterVal = APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset);
      break;
    case RISCV::BLT:
    case RISCV::BLTU:
      getSupportInstBuilder(*this, MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(ADDIOp))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(CounterInitInfo.StrideVal.getZExtValue());
      ExitingValues[CounterReg] =
          APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset + NIter);
      MinCounterVal =
          APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset + 1);
      break;
    case RISCV::BGE:
    case RISCV::BGEU:
      getSupportInstBuilder(*this, MBB, Pos,
                            MBB.getParent()->getFunction().getContext(),
                            InstrInfo.get(ADDIOp))
          .addReg(CounterReg, RegState::Define)
          .addReg(CounterReg)
          .addImm(-CounterInitInfo.StrideVal.getSExtValue());
      ExitingValues[CounterReg] =
          APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset);
      MinCounterVal = APInt(getRegBitWidth(CounterReg, IGC), RegCounterOffset);
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

  MachineInstr *generateFinalInst(InstructionGenerationContext &IGC,
                                  unsigned LastInstrOpc) const override {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto MIB =
        getSupportInstBuilder(*this, IGC.MBB, IGC.Ins,
                              IGC.MBB.getParent()->getFunction().getContext(),
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
                                    LLVMState &State) const override {
    // TODO: return actual minimum alignment of Reg.
    return 16u;
  }

  unsigned getRegBitWidth(MCRegister Reg,
                          InstructionGenerationContext &IGC) const override {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
    auto VLEN =
        ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>().getVLEN();
    return snippy::getRegBitWidth(Reg, ST.getXLen(), VLEN);
  }

  MCRegister regIndexToMCReg(InstructionGenerationContext &IGC, unsigned RegIdx,
                             RegStorageType Storage) const override {
    const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
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

  unsigned
  getSpillSizeInBytes(MCRegister Reg,
                      InstructionGenerationContext &IGC) const override {
    unsigned RegSize = getRegBitWidth(Reg, IGC) / RISCV_CHAR_BIT;
    auto &ProgCtx = IGC.ProgCtx;
    auto Alignment = getSpillAlignmentInBytes(Reg, ProgCtx.getLLVMState());
    assert(Alignment && "Alignment size can't be zero");
    // Get the least number of alignment sizes that fully fits register.
    return Alignment * divideCeil(RegSize, Alignment);
  }

  std::vector<std::string> getCallerSavedRegGroups() const override {
    return {regTypeToString(RegType::X).str(),
            regTypeToString(RegType::F).str(),
            regTypeToString(RegType::V).str()};
  }

  std::vector<std::string> getCallerSavedLiveRegGroups() const override {
    return {regTypeToString(RegType::X).str(),
            regTypeToString(RegType::F).str()};
  }

  std::vector<MCRegister>
  getCallerSavedRegs(const MachineFunction &MF,
                     ArrayRef<std::string> RegGroups) const override {
    using namespace ::RISCV;

    if (RegGroups.empty())
      return {};

    const auto &SubTgt = MF.getSubtarget();
    std::vector<MCRegister> CallerRegs;
    if (llvm::is_contained(RegGroups, regTypeToString(RegType::X)))
      CallerRegs.insert(CallerRegs.end(),
                        {X1,                             /* Return address */
                         X5, X6, X7, X28, X29, X30, X31, /* Temporaries */
                         X10, X11, /* Function arguments/return values */
                         X12, X13, X14, X15, X16,
                         X17 /* Function arguments */});

    if (llvm::is_contained(RegGroups, regTypeToString(RegType::F))) {
      if (SubTgt.hasFeature(FeatureStdExtD))
        CallerRegs.insert(CallerRegs.end(),
                          {F0_D,  F1_D,  F2_D,  F3_D,  F4_D,  F5_D,  F6_D,
                           F7_D,  F28_D, F29_D, F30_D, F31_D, /* Temporaries */
                           F10_D, F11_D, /* Arguments / return values */
                           F12_D, F13_D, F14_D, F15_D, F16_D, F17_D,
                           /* Arguments */});
      else if (SubTgt.hasFeature(FeatureStdExtF))
        CallerRegs.insert(CallerRegs.end(),
                          {F0_F,  F1_F,  F2_F,  F3_F,  F4_F,  F5_F,  F6_F,
                           F7_F,  F28_F, F29_F, F30_F, F31_F, /* Temporaries */
                           F10_F, F11_F, /* Arguments / return values */
                           F12_F, F13_F, F14_F, F15_F, F16_F, F17_F,
                           /* Arguments */});
    }

    auto HasVectorRegs = [&MF] {
      return llvm::any_of(MF, [](auto &&MBB) {
        return llvm::any_of(MBB,
                            [](auto &&MI) { return isRVV(MI.getOpcode()); });
      });
    };
    if (llvm::is_contained(RegGroups, regTypeToString(RegType::V))) {
      if (SubTgt.hasFeature(FeatureStdExtV) && HasVectorRegs())
        CallerRegs.insert(CallerRegs.end(),
                          {
                              V0,  V1,  V2,  V3,  V4,  V5,  V6,  V7,
                              V8,  V9,  V10, V11, V12, V13, V14, V15,
                              V16, V17, V18, V19, V20, V21, V22, V23,
                              V24, V25, V26, V27, V28, V29, V30, V31,
                          });
    }
    return CallerRegs;
  }

  std::vector<MCRegister>
  getRegsPreservedByABI(const MCSubtargetInfo &SubTgt) const override {
    using namespace ::RISCV;

    std::vector<MCRegister> PreservedRegs{
        X1 /* Return address */, X3 /* Global pointer */,
        X4 /* Thread pointer */, X8 /* Frame pointer */,
        /* Saved registers (s1-s11) */
        X9, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27};

    if (SubTgt.hasFeature(FeatureStdExtD))
      PreservedRegs.insert(PreservedRegs.end(),
                           {/* Saved registers (fs0, fs1, fs2-fs11) */ F8_D,
                            F9_D, F18_D, F19_D, F20_D, F21_D, F22_D, F23_D,
                            F24_D, F25_D, F26_D, F27_D});
    else if (SubTgt.hasFeature(FeatureStdExtF))
      PreservedRegs.insert(PreservedRegs.end(),
                           {/* Saved registers (fs0, fs1, fs2-fs11) */ F8_F,
                            F9_F, F18_F, F19_F, F20_F, F21_F, F22_F, F23_F,
                            F24_F, F25_F, F26_F, F27_F});

    return PreservedRegs;
  }

  // X3 is a thread pointer and X4 is a global pointer. We must preserve them so
  // they have valid values when we call external functions.
  std::vector<MCRegister> getGlobalStateRegs() const override {
    return {RISCV::X3, RISCV::X4};
  }

  const MCRegisterClass &
  getRegClassSuitableForSP(const MCRegisterInfo &RI) const override {
    return RI.getRegClass(RISCV::GPRNoX0RegClassID);
  }

  std::function<bool(MCRegister)>
  filterSuitableRegsForStackPointer() const override {

    /* X6 is excluded, because it is the default destination for AUIPC in
     * tailcalls */
    return [](auto Reg) {
      return Reg == RISCV::X0 || Reg == RISCV::X1 || Reg == RISCV::X6;
    };
  }

  MCRegister getStackPointer() const override { return RISCV::X2; }

  bool isRegClassSupported(MCRegister Reg) const override {
    return RISCV::GPRRegClass.contains(Reg) ||
           RISCV::FPR16RegClass.contains(Reg) ||
           RISCV::FPR32RegClass.contains(Reg) ||
           RISCV::FPR64RegClass.contains(Reg) ||
           RISCV::VRRegClass.contains(Reg);
  }

  void generateSpillToStack(InstructionGenerationContext &IGC, MCRegister Reg,
                            MCRegister SP) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    assert(ProgCtx.stackEnabled() &&
           "An attempt to generate spill but stack was not enabled.");
    auto &State = ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();
    getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(-static_cast<int64_t>(getSpillSizeInBytes(Reg, IGC)));

    storeRegToAddrInReg(IGC, SP, Reg);
  }

  void generateReloadFromStack(InstructionGenerationContext &IGC,
                               MCRegister Reg, MCRegister SP) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    assert(ProgCtx.stackEnabled() &&
           "An attempt to generate reload but stack was not enabled.");
    auto &State = ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();

    loadRegFromAddrInReg(IGC, SP, Reg);
    getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(getSpillSizeInBytes(Reg, IGC));
  }

  void generatePopNoReload(InstructionGenerationContext &IGC,
                           MCRegister Reg) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    assert(ProgCtx.stackEnabled() &&
           "An attempt to generate stack pop but stack was not enabled.");
    auto &State = ProgCtx.getLLVMState();
    auto SP = ProgCtx.getStackPointer();
    const auto &InstrInfo = State.getInstrInfo();
    auto &Ctx = State.getCtx();

    getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::ADDI))
        .addDef(SP)
        .addReg(SP)
        .addImm(getSpillSizeInBytes(Reg, IGC));
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target,
                             bool AsSupport) const override {
    return generateCall(IGC, Target, AsSupport, RISCV::JAL);
  }

  MachineInstr *loadSymbolAddress(InstructionGenerationContext &IGC,
                                  unsigned DestReg,
                                  const GlobalValue *Target) const override {
    auto &Ins = IGC.Ins;
    auto &MBB = IGC.MBB;
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    MachineFunction *MF = MBB.getParent();

    // Cannot emit PseudoLLA here, because this pseudo instruction is expanded
    // by RISCVPreRAExpandPseudo pass, which runs before register allocation.
    // That's why create auipc + addi pair manually.

    MachineInstr *MIAUIPC =
        getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::AUIPC))
            .addDef(DestReg)
            .addGlobalAddress(Target, 0, RISCVII::MO_PCREL_HI);
    MCSymbol *AUIPCSymbol = MF->getContext().createNamedTempSymbol("pcrel_hi");
    MIAUIPC->setPreInstrSymbol(*MF, AUIPCSymbol);

    return getSupportInstBuilder(*this, MBB, Ins, Ctx,
                                 InstrInfo.get(RISCV::ADDI))
        .addDef(DestReg)
        .addReg(DestReg)
        .addSym(AUIPCSymbol, RISCVII::MO_PCREL_LO);
  }

  MachineInstr *generateFenceI(InstructionGenerationContext &IGC) const {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    return getSupportInstBuilder(*this, IGC.MBB, IGC.Ins, Ctx,
                                 InstrInfo.get(RISCV::FENCE_I));
  }

  MachineInstr *
  generateMemoryBarrier(InstructionGenerationContext &IGC) const override {
    return generateFenceI(IGC);
  }

  MachineInstr *generateJAL(InstructionGenerationContext &IGC,
                            const Function &Target, bool AsSupport) const {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    // Despite PseudoCALL gets expanded by RISCVMCCodeEmitter to JALR
    // instruction, it has chance to be relaxed back to JAL by linker.
    return getInstBuilder(AsSupport, *this, IGC.MBB, IGC.Ins, Ctx,
                          InstrInfo.get(RISCV::PseudoCALL))
        .addGlobalAddress(&Target, 0, RISCVII::MO_CALL);
  }

  MachineInstr *generateJALR(InstructionGenerationContext &IGC,
                             const Function &Target, bool AsSupport) const {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRJALRRegClassID);
    auto RP = IGC.pushRegPool();
    auto Reg = getNonZeroReg("scratch register for storing function address",
                             RI, RegClass, *RP, IGC.MBB);
    loadSymbolAddress(IGC, Reg, &Target);
    return getInstBuilder(AsSupport, *this, IGC.MBB, IGC.Ins, Ctx,
                          InstrInfo.get(RISCV::PseudoCALLIndirect))
        .addReg(Reg);
  }

  MachineInstr *generateCall(InstructionGenerationContext &IGC,
                             const Function &Target, bool AsSupport,
                             unsigned PreferredCallOpcode) const override {
    assert(isCall(PreferredCallOpcode) && "Expected call here");
    switch (PreferredCallOpcode) {
    case RISCV::JAL:
      return generateJAL(IGC, Target, AsSupport);
    case RISCV::JALR:
      return generateJALR(IGC, Target, AsSupport);
    default:
      snippy::fatal("Unsupported call instruction");
    }
  }

  MachineInstr *generateTailCall(InstructionGenerationContext &IGC,
                                 const Function &Target) const override {
    auto &ProgCtx = IGC.ProgCtx;
    const auto &InstrInfo = ProgCtx.getLLVMState().getInstrInfo();
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    return getSupportInstBuilder(*this, IGC.MBB, IGC.Ins, Ctx,
                                 InstrInfo.get(RISCV::PseudoTAIL))
        .addGlobalAddress(&Target, 0, RISCVII::MO_CALL);
  }

  MachineInstr *
  generateReturn(InstructionGenerationContext &IGC) const override {
    auto &State = IGC.ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB =
        getSupportInstBuilder(*this, IGC.MBB, IGC.Ins,
                              IGC.MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(RISCV::PseudoRET));
    return MIB;
  }

  MachineInstr *generateNop(InstructionGenerationContext &IGC) const override {
    auto &State = IGC.ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    auto MIB =
        getSupportInstBuilder(*this, IGC.MBB, IGC.Ins,
                              IGC.MBB.getParent()->getFunction().getContext(),
                              InstrInfo.get(RISCV::ADDI), RISCV::X0)
            .addReg(RISCV::X0)
            .addImm(0);
    return MIB;
  }

  unsigned getTransformSequenceLength(InstructionGenerationContext &IGC,
                                      APInt OldValue, APInt NewValue,
                                      MCRegister Register) const override {
    if (!RISCV::GPRRegClass.contains(Register))
      snippy::fatal("transform of value in register is supported only for GPR");

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
    return getWriteValueSequenceLength(IGC, ValueToWrite, Register) + 1;
  }

  void transformValueInReg(InstructionGenerationContext &IGC, APInt OldValue,
                           APInt NewValue, MCRegister Register) const override {
    if (!RISCV::GPRRegClass.contains(Register))
      snippy::fatal("transform of value in register is supported only for GPR");

    auto RP = IGC.pushRegPool();
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;

    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    const auto &InstrInfo = State.getInstrInfo();

    if (NewValue.eq(OldValue))
      return;

    // Materialization sequence should not touch Register.
    RP->addReserved(Register, AccessMaskBit::W);

    if (!hasNonZeroRegAvailable(RegClass, *RP, AccessMaskBit::W)) {
      writeValueToReg(IGC, NewValue, Register);
      return;
    }

    // First need to choose another not X0 reg to materialize
    // difference value in.
    auto ScratchReg = getNonZeroReg("scratch register for transforming value",
                                    RI, RegClass, *RP, MBB);

    // Choose final operation based on value relation.
    bool WillUseAdd = NewValue.ugt(OldValue);
    bool Overflowed = false;
    auto ValueToWrite =
        APInt(WillUseAdd ? NewValue.usub_ov(OldValue, Overflowed)
                         : OldValue.usub_ov(NewValue, Overflowed));
    writeValueToReg(IGC, ValueToWrite, ScratchReg);
    assert(!Overflowed && "Expression expect to not overflow");
    assert(
        getTransformSequenceLength(IGC, OldValue, NewValue, Register) ==
            (getWriteValueSequenceLength(IGC, ValueToWrite, ScratchReg) + 1) &&
        "Generated sequence length does not match expected one");
    getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                          InstrInfo.get(WillUseAdd ? RISCV::ADD : RISCV::SUB),
                          Register)
        .addReg(Register)
        .addReg(ScratchReg);
  }

  void loadEffectiveAddressInReg(InstructionGenerationContext &IGC,
                                 MCRegister Register, uint64_t BaseAddr,
                                 uint64_t Stride,
                                 MCRegister IndexReg) const override {
    auto RP = IGC.pushRegPool();
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    assert(RISCV::GPRRegClass.contains(Register, IndexReg) &&
           "Only GPR registers are supported");

    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    const auto &RI = State.getRegInfo();
    const auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    const auto &InstrInfo = State.getInstrInfo();

    auto XRegBitSize = getRegBitWidth(Register, IGC);
    writeValueToReg(IGC, APInt(XRegBitSize, Stride), Register);
    getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                          InstrInfo.get(RISCV::MUL), Register)
        .addReg(Register)
        .addReg(IndexReg);

    RP->addReserved(Register);
    if (!hasNonZeroRegAvailable(RegClass, *RP))
      snippy::fatal("Can't find suitable scratch register");

    auto AddrReg =
        getNonZeroReg("Scratch register for BaseAddr", RI, RegClass, *RP, MBB);

    writeValueToReg(IGC, APInt(XRegBitSize, BaseAddr), AddrReg);
    getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                          InstrInfo.get(RISCV::ADD), Register)
        .addReg(Register)
        .addReg(AddrReg);
  }

  MachineOperand createOperandForOpType(unsigned Opcode,
                                        const ImmediateHistogramSequence *IH,
                                        unsigned OperandType,
                                        const StridedImmediate &StridedImm,
                                        const TargetMachine &TM) const {
    // NOTE: need to be in sync with
    // llvm/lib/Target/RISCV/MCTargetDesc/RISCVBaseInfo.h(RISCVOp)
    // llvm/lib/Target/RISCV/RISCVInstrInfo.cpp
    // (RISCVInstrInfo::verifyInstruction)
    switch (OperandType) {
    default:
      snippy::fatal("Requested generation for an unexpected operand type.");
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
    case RISCVOp::OPERAND_UIMM5: {
      // Has to be in sync with llvm/lib/Target/RISCV/RISCVInstrInfo.td
      // Specifically with WriteSysRegImm and SwapSysRegImm classes.
      bool IsFRMSysRegWrite =
          (Opcode == RISCV::WriteFRMImm || Opcode == RISCV::SwapFRMImm);
      // When the immediate histogram is not specified and the operation writes
      // to FRM only generate non-reserved values.
      if (!IH && IsFRMSysRegWrite) {
        using namespace RISCVFPRndMode;
        // When generating immediates for operations that modify the FRM
        // we should generate only valid rounding modes and not use the dynamic
        // mode, since that value is reserved.
        return MachineOperand::CreateImm(
            static_cast<int>(snippy::selectFrom(RNE, RTZ, RDN, RUP, RMM)));
      }
      return MachineOperand::CreateImm(genImmUINT<5>(IH, StridedImm));
    }
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
      snippy::fatal("AVL operand generation is not supported. Probably "
                    "snippy still does "
                    "not support vector instructions generation.");
    case RISCVOp::OPERAND_FRMARG: {
      // Floating-point operations use either a static rounding mode encoded in
      // the instruction, or a dynamic rounding mode held in frm.
      // 000 - RNE (Round to Nearest, ties to Even)
      // 001 - RTZ (Round towards Zero)
      // 010 - RDN (Round Down)
      // 011 - RUP (Round Up)
      // 100 - RMM (Round to Nearest, ties to Max Magnitude)
      // 101 - <reserved>
      // 110 - <reserved>
      // 111 - DYN (In instruction’s rm field, selects dynamic rounding mode)
      return MachineOperand::CreateImm([&]() -> int {
        using namespace RISCVFPRndMode;
        // If the immediate histogram is specified then sample it.
        if (IH)
          return genImmInInterval<RNE, DYN>(*IH);
        // Otherwise generate only valid static rounding modes or use dynamic
        // one.
        return static_cast<int>(
            snippy::selectFrom(RNE, RTZ, RDN, RUP, RMM, DYN));
      }());
    }
    }
  }

  MachineOperand genTargetOpForOpcode(unsigned Opcode, unsigned OperandType,
                                      const StridedImmediate &StridedImm,
                                      SnippyProgramContext &ProgCtx,
                                      const CommonPolicyConfig &Cfg) const {
    const auto &TM = ProgCtx.getLLVMState().getTargetMachine();
    const auto &OpcSetting =
        Cfg.ImmHistMap.getConfigForOpcode(Opcode, ProgCtx.getOpcodeCache());
    if (OpcSetting.isUniform())
      return createOperandForOpType(Opcode, /*IH=*/nullptr, OperandType,
                                    StridedImm, TM);
    const auto &Seq = OpcSetting.getSequence();
    return createOperandForOpType(Opcode, &Seq, OperandType, StridedImm, TM);
  }

  MachineOperand
  generateTargetOperand(SnippyProgramContext &ProgCtx,
                        const CommonPolicyConfig &Cfg, unsigned Opcode,
                        unsigned OperandType,
                        const StridedImmediate &StridedImm) const override {
    const auto &IHV = Cfg.ImmHistogram;
    if (IHV.holdsAlternative<ImmediateHistogramRegEx>())
      return genTargetOpForOpcode(Opcode, OperandType, StridedImm, ProgCtx,
                                  Cfg);

    const auto *IH = [&]() -> const ImmediateHistogramSequence * {
      // Disable histogram for loads and stores
      if (isSupportedLoadStore(Opcode))
        return nullptr;
      const auto *Sequence = &IHV.get<ImmediateHistogramSequence>();
      // Fallback case for empty immediate histogram -> generate immediates
      // uniformly.
      return Sequence->empty() ? nullptr : Sequence;
    }();

    return createOperandForOpType(Opcode, IH, OperandType, StridedImm,
                                  ProgCtx.getLLVMState().getTargetMachine());
  }

  AccessMaskBit getCustomAccessMaskForOperand(const MCInstrDesc &InstrDesc,
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
  breakDownAddr(InstructionGenerationContext &IGC, AddressInfo AddrInfo,
                const MachineInstr &MI, unsigned AddrIdx) const override {
    auto Opcode = MI.getOpcode();
    assert((isSupportedLoadStore(Opcode) || isAtomicAMO(Opcode) ||
            isLrInstr(Opcode) || isScInstr(Opcode)) &&
           "Requested addr calculation for unsupported instruction");
    assert(AddrIdx == 0 && "RISC-V supports only one address per instruction");

    auto &ProgCtx = IGC.ProgCtx;
    const auto &TM = ProgCtx.getLLVMState().getTargetMachine();
    if (isAtomicAMO(Opcode) || isLrInstr(Opcode) || isScInstr(Opcode) ||
        isRVVUnitStrideLoadStore(Opcode) || isRVVUnitStrideFFLoad(Opcode) ||
        isRVVUnitStrideSegLoadStore(Opcode) || isRVVWholeRegLoadStore(Opcode) ||
        isRVVUnitStrideMaskLoadStore(Opcode) || isZicbo(Opcode)) {
      auto &State = ProgCtx.getLLVMState();
      auto &RI = State.getRegInfo();
      const auto &AddrReg = getMemOperand(MI);
      auto AddrValue = AddrInfo.Address;
      const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
      auto Part = AddressPart{AddrReg, APInt(ST.getXLen(), AddrValue), RI};

      if (isAtomicAMO(Opcode) || isLrInstr(Opcode) || isScInstr(Opcode) ||
          isRVVWholeRegLoadStore(Opcode) || isZicbo(Opcode))
        return std::make_pair<AddressParts, MemAddresses>(
            {std::move(Part)}, {uintToTargetXLen(is64Bit(TM), AddrValue)});

      assert(isRVV(Opcode));
      auto &TgtCtx =
          ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
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
      return breakDownAddrForRVVStrided(AddrInfo, MI, IGC, is64Bit(TM));
    if (isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode))
      return breakDownAddrForRVVIndexed(AddrInfo, MI, IGC, is64Bit(TM));
    return breakDownAddrForInstrWithImmOffset(AddrInfo, MI, IGC, is64Bit(TM));
  }

  unsigned getWriteValueSequenceLength(InstructionGenerationContext &IGC,
                                       APInt Value,
                                       MCRegister Register) const override {
    if (RISCV::VRRegClass.contains(Register))
      snippy::fatal("Not implemented for RVV regs yet");

    return getIntMatInstrSeq(Value, IGC).size();
  }

  void writeValueToReg(InstructionGenerationContext &IGC, APInt Value,
                       unsigned DstReg) const override {
    // TODO: Instruction sequence generation for RVV has not been implemented
    // yet, so we write directly.
    if (RISCV::VRRegClass.contains(DstReg)) {
      rvvWriteValue(IGC, Value, DstReg);
      return;
    }

    SmallVector<MCInst> InstrsForWrite;
    generateWriteValueSeq(IGC, Value, DstReg, InstrsForWrite);
    addGeneratedInstrsToBB(IGC, InstrsForWrite, *this);
  }

  void copyRegToReg(InstructionGenerationContext &IGC, MCRegister Rs,
                    MCRegister Rd) const override {
    assert(RISCV::GPRRegClass.contains(Rs) && RISCV::GPRRegClass.contains(Rd) &&
           "Both src and dst registers must be GPR");
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    const auto &InstrInfo = State.getInstrInfo();
    getSupportInstBuilder(*this, MBB, Ins,
                          MBB.getParent()->getFunction().getContext(),
                          InstrInfo.get(RISCV::ADD), Rd)
        .addReg(Rs)
        .addReg(RISCV::X0);
  }

  void loadRegFromAddrInReg(InstructionGenerationContext &IGC,
                            MCRegister AddrReg, MCRegister Reg) const override {
    const auto LoadInstr = generateLoadRegFromAddrInReg(IGC, AddrReg, Reg);
    addGeneratedInstrsToBB(IGC, {LoadInstr}, *this);
  }

  MCInst generateLoadRegFromAddrInReg(InstructionGenerationContext &IGC,
                                      MCRegister AddrReg,
                                      MCRegister Reg) const {
    assert(RISCV::GPRRegClass.contains(AddrReg) &&
           "Expected address register be GPR");

    if (RISCV::GPRRegClass.contains(Reg)) {
      const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
      auto LoadOp = ST.getXLen() == 32 ? RISCV::LW : RISCV::LD;
      return MCInstBuilder(LoadOp).addReg(Reg).addReg(AddrReg).addImm(0);
    }
    if (RISCV::FPR16RegClass.contains(Reg))
      return MCInstBuilder(RISCV::FLH).addReg(Reg).addReg(AddrReg).addImm(0);
    if (RISCV::FPR32RegClass.contains(Reg))
      return MCInstBuilder(RISCV::FLW).addReg(Reg).addReg(AddrReg).addImm(0);
    if (RISCV::FPR64RegClass.contains(Reg))
      return MCInstBuilder(RISCV::FLD).addReg(Reg).addReg(AddrReg).addImm(0);
    if (RISCV::VRRegClass.contains(Reg))
      return MCInstBuilder(RISCV::VL1RE8_V).addReg(Reg).addReg(AddrReg);
    snippy::fatal(
        formatv("Cannot generate load from memory for register {0}", Reg));
    return {};
  }

  void loadRegFromAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                       MCRegister Reg) const override {
    SmallVector<MCInst> InstrsForWrite;
    generateLoadRegFromAddr(IGC, Addr, Reg, InstrsForWrite);
    addGeneratedInstrsToBB(IGC, InstrsForWrite, *this);
  }

  void generateLoadRegFromAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                               MCRegister Reg,
                               SmallVectorImpl<MCInst> &Insts) const {
    auto &MBB = IGC.MBB;
    auto &RP = IGC.getRegPool();
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    auto XScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                     RP, MBB, AccessMaskBit::SRW);
    // Form address in scratch register.
    generateWriteValueSeq(IGC, APInt(getRegBitWidth(XScratchReg, IGC), Addr),
                          XScratchReg, Insts);
    Insts.push_back(generateLoadRegFromAddrInReg(IGC, XScratchReg, Reg));
  }

  void storeRegToAddrInReg(InstructionGenerationContext &IGC,
                           MCRegister AddrReg, MCRegister Reg,
                           unsigned BytesToWrite = 0) const {
    assert(RISCV::GPRRegClass.contains(AddrReg) &&
           "Expected address register be GPR");
    auto &MBB = IGC.MBB;
    auto &Ins = IGC.Ins;
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &Ctx = State.getCtx();
    const auto &InstrInfo = State.getInstrInfo();

    if (RISCV::GPRRegClass.contains(Reg)) {
      auto StoreOp = getStoreOpcode(BytesToWrite ? BytesToWrite * RISCV_CHAR_BIT
                                                 : getRegBitWidth(Reg, IGC));
      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(StoreOp))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR32RegClass.contains(Reg) ||
               RISCV::FPR16RegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, IGC));
      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::FSW))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::FPR64RegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, IGC));
      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::FSD))
          .addReg(Reg)
          .addReg(AddrReg)
          .addImm(0);
    } else if (RISCV::VRRegClass.contains(Reg)) {
      assert(BytesToWrite == 0 ||
             BytesToWrite * RISCV_CHAR_BIT == getRegBitWidth(Reg, IGC));
      getSupportInstBuilder(*this, MBB, Ins, Ctx, InstrInfo.get(RISCV::VS1R_V))
          .addReg(Reg)
          .addReg(AddrReg);
    } else {
      snippy::fatal(
          formatv("Cannot generate store to memory for register {0}", Reg));
    }
  }

  void storeRegToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                      MCRegister Reg, unsigned BytesToWrite) const override {
    auto &MBB = IGC.MBB;
    auto RP = IGC.pushRegPool();
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &RI = State.getRegInfo();
    auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
    RP->addReserved(getFirstPhysReg(Reg, RI), MBB);
    auto ScratchReg = getNonZeroReg("scratch register for addr", RI, RegClass,
                                    *RP, MBB, AccessMaskBit::SRW);
    auto XRegBitSize = getRegBitWidth(ScratchReg, IGC);

    writeValueToReg(IGC, APInt(XRegBitSize, Addr), ScratchReg);
    storeRegToAddrInReg(IGC, ScratchReg, Reg, BytesToWrite);
  }

  void storeValueToAddr(InstructionGenerationContext &IGC, uint64_t Addr,
                        APInt Value) const override {
    auto &ProgCtx = IGC.ProgCtx;
    auto &State = ProgCtx.getLLVMState();
    auto &MBB = IGC.MBB;
    auto &RP = IGC.getRegPool();
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
      if (ValueRegBitSize > getRegBitWidth(RegForValue, IGC))
        snippy::fatal(State.getCtx(), "Selfcheck error ",
                      "selfcheck is not implemented for rv32 with D ext");
    }
    writeValueToReg(IGC, Value.zext(getRegBitWidth(RegForValue, IGC)),
                    RegForValue);
    assert(ValueRegBitSize % RISCV_CHAR_BIT == 0);
    storeRegToAddr(IGC, Addr, RegForValue, ValueRegBitSize / RISCV_CHAR_BIT);
  }

  size_t getAccessSize(unsigned Opcode, SnippyProgramContext &ProgCtx,
                       const MachineBasicBlock &MBB) const {
    assert(countAddrsToGenerate(Opcode) &&
           "Requested access size calculation, but instruction does not access "
           "memory");

    if (!isRVV(Opcode))
      return getDataElementWidth(Opcode);

    auto &TgtCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();

    auto VL = TgtCtx.getVL(MBB);
    if (VL == 0 && !isRVVWholeRegLoadStore(Opcode))
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

  InstrMemAccessInfo
  getAccessSizeAndAlignment(SnippyProgramContext &ProgCtx, unsigned Opcode,
                            const MachineBasicBlock &MBB) const override {
    unsigned SEW = 0;
    auto &TgtCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    if (TgtCtx.hasActiveRVVMode(MBB))
      SEW = static_cast<unsigned>(TgtCtx.getSEW(MBB));

    auto MisalignedAccessMode = getMisalignedAccessMode();
    bool DisableMisalign =
        (MisalignedAccessMode == DisableMisalignedAccessMode::All ||
         (MisalignedAccessMode == DisableMisalignedAccessMode::AtomicsOnly &&
          (isAtomicAMO(Opcode) || isScInstr(Opcode) || isLrInstr(Opcode))));

    auto NaturalAlignment = getLoadStoreNaturalAlignment(Opcode, SEW);
    if (!DisableMisalign &&
        NaturalAlignment == 0) // happens for atomic instructions for example
      NaturalAlignment = 1;

    return {getAccessSize(Opcode, ProgCtx, MBB), NaturalAlignment,
            !DisableMisalign};
  }

  void
  excludeFromMemRegsForOpcode(unsigned Opcode, const MCRegisterInfo &RI,
                              SmallVectorImpl<Register> &Regs) const override {
    if (!isCLoadStore(Opcode) && !isCFPLoadStore(Opcode)) {
      getPhysRegsFromUnit(RISCV::X0, RI, Regs);
      return;
    }

    SmallVector<Register, 32> Result;
    if (isCSPRelativeLoadStore(Opcode) || isCFPSPRelativeLoadStore(Opcode)) {
      copy_if(getAddrRegClass(), std::back_inserter(Result),
              [](Register Reg) { return Reg != RISCV::X2; });
    } else {
      copy_if(getAddrRegClass(), std::back_inserter(Result), [](Register Reg) {
        return !is_contained(RISCV::GPRCRegClass, Reg);
      });
    }

    Regs.clear();
    transform(Result, std::back_inserter(Regs), [&RI, this](Register Reg) {
      SmallVector<Register> PhRegs;
      getPhysRegsFromUnit(Reg, RI, PhRegs);
      assert(PhRegs.size() == 1 &&
             "Expect only one reg unit for RISC-V addr reg class");
      return PhRegs.front();
    });
  }

  std::vector<Register> excludeRegsForOperand(InstructionGenerationContext &IGC,
                                              const MCRegisterClass &RC,
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

  std::vector<Register> includeRegs(unsigned Opcode,
                                    const MCRegisterClass &RC) const override {
    if (RC.getID() == RISCV::VMV0RegClass.getID() &&
        !isRVVuseV0RegExplicitly(Opcode))
      return {RISCV::NoRegister};
    return {};
  }

  // Reserve a register (register group) that has already been used as the
  // destination or memory if it is needed for the given opcode.
  // (changes in the GeneratorContext may cause problems
  //  in operands pregeneration)
  void reserveRegsIfNeeded(InstructionGenerationContext &IGC, unsigned Opcode,
                           bool isDst, bool isMem,
                           Register Reg) const override {
    auto &MBB = IGC.MBB;
    const auto &TgtCtx =
        IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    auto &RP = IGC.getRegPool();
    // For vector indexed segment loads, the destination vector register groups
    // cannot overlap the source vector register group (specifed by vs2), else
    // the instruction encoding is reserved.
    if ((isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) &&
        isDst) {
      assert(RISCV::VRRegClass.contains(Reg) &&
             "Dst reg in rvv indexed load/store instruction must be vreg");

      auto [Mult, Fractional] = TgtCtx.decodeVLMUL(TgtCtx.getLMUL(MBB));
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
    // FIXME: The vector integer extension instructions (zero- or sign-extend)
    // need special handling. Overlaps in the highest-numbered part of the
    // destination register group are allowed. For example, when LMUL=8,
    // vzext.vf4 v0, v6 is legal, but a source of v0, v2, or v4 is not.
    // (riscv-v-spec-1.0, "Vector Operands" section)
    if (isRVVExt(Opcode) && isDst)
      RP.addReserved(Reg);
    if (isRVVWidening(Opcode) && isDst)
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
           isAtomicAMO(Opcode) || isCLoadStore(Opcode) || isZicbo(Opcode) ||
           isCFPLoadStore(Opcode) || isFence(Opcode);
  }

  bool canInitializeOperand(const MCInstrDesc &InstrDesc,
                            unsigned OpIndex) const override {
    auto Opcode = InstrDesc.getOpcode();
    // We can't initialize registers before control flow instructions
    if (isBaseCFInstr(Opcode) || isCall(Opcode) || Opcode == RISCV::AUIPC)
      return false;
    assert(InstrDesc.getNumOperands() > OpIndex &&
           "This must be the index of the operand");
    auto Operand = InstrDesc.operands()[OpIndex];
    // Registers that are memory addresses can't be initialized.
    // Their type is OperandType::OPERAND_MEMORY.
    if (Operand.OperandType != MCOI::OperandType::OPERAND_REGISTER)
      return false;
    // Both indexed and stride rvv load/stores has additional 'memory operand'
    // right after main address operand.
    if (isRVVIndexedLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode) ||
        isRVVStridedLoadStore(Opcode) || isRVVStridedSegLoadStore(Opcode)) {
      // This operand needs special handling and cannot be initialized.
      auto MemOpIdx = getMemOperandIdx(InstrDesc);
      if (MemOpIdx + 1 == OpIndex)
        return false;
    }
    return true;
  }
  unsigned getImmOffsetAlignmentForMemAccessInst(
      const MCInstrDesc &InstrDesc) const override {
    auto Opcode = InstrDesc.getOpcode();
    // Compressed instructions' offset is required to be aligned to element
    // width
    if (isCLoadStore(Opcode)) {
      return getDataElementWidth(Opcode);
    }
    return 1;
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

  void rvvWriteValue(InstructionGenerationContext &IGC, APInt Value,
                     unsigned DstReg) const;

  void rvvUnsafeWriteValueUsingXReg(InstructionGenerationContext &IGC,
                                    APInt Value, unsigned DstReg) const;

  void rvvWriteValueUsingXReg(InstructionGenerationContext &IGC, APInt Value,
                              unsigned DstReg) const;

  std::optional<RVVModeInfo>
  rvvWriteValueUsingXRegAndGetOldMode(InstructionGenerationContext &IGC,
                                      APInt Value, unsigned DstReg) const;

  void rvvWriteValueToV0UsingVReg(InstructionGenerationContext &IGC,
                                  APInt Value) const;

  void rvvWriteValueUsingLoad(InstructionGenerationContext &IGC, APInt Value,
                              unsigned DstReg) const;

  void generateWriteValueFP(InstructionGenerationContext &IGC, APInt Value,
                            unsigned DstReg,
                            SmallVectorImpl<MCInst> &Insts) const;

  // NOTE: DesiredOpcode is expected to be any mode changing opcode
  // (RISCV::VSETVL, RISCV::VSETVLI, RISCV::VSETIVLI) or
  // If !DesiredOpcode.has_value() this means that the user does not really care
  // which instruction to use. In case of the latter the implementaion is free
  // to chose any suitable opcode
  void rvvGenerateModeSwitchAndUpdateContext(
      const MCInstrInfo &InstrInfo, InstructionGenerationContext &IGC,
      std::optional<unsigned> DesiredOpcode = {}) const;

  void generateVTypeChange(InstructionGenerationContext &IGC,
                           const MCInstrInfo &InstrInfo,
                           const RVVModeInfo &NewRVVMode,
                           std::optional<unsigned> DesiredOpcode = {}) const;

  void generateRVVModeUpdate(InstructionGenerationContext &IGC,
                             const MCInstrInfo &InstrInfo,
                             const RVVModeInfo &NewRVVMode,
                             std::optional<unsigned> DesiredOpcode = {}) const;

  bool generateRVVModeUpdateIfNeeded(
      InstructionGenerationContext &IGC, const MCInstrInfo &InstrInfo,
      const RVVModeInfo &NewRVVMode,
      std::optional<unsigned> DesiredOpcode = {}) const;

  void generateV0MaskUpdate(InstructionGenerationContext &IGC, const APInt VM,
                            const MCInstrInfo &InstrInfo) const;

  void updateRVVConfig(InstructionGenerationContext &IGC,
                       const MachineInstr &MI) const;

  // NOTE: VSET{I}VL{I} functions are expected to be called by
  // generateVTypeChange only
  void generateVSETIVLI(InstructionGenerationContext &IGC,
                        const MCInstrInfo &InstrInfo, unsigned VTYPE,
                        unsigned VL, bool SupportMarker) const;

  void generateVSETVLI(InstructionGenerationContext &IGC,
                       const MCInstrInfo &InstrInfo, unsigned VTYPE,
                       unsigned VL, bool SupportMarker) const;

  void generateVSETVL(InstructionGenerationContext &IGC,
                      const MCInstrInfo &InstrInfo, unsigned VTYPE, unsigned VL,
                      bool SupportMarker) const;

  bool isFloatingPoint(MCRegister Reg) const override {
    return snippy::isFloatingPointReg(Reg);
  }

  bool isFloatingPoint(const MCInstrDesc &InstrDesc) const override {
    return snippy::isFloatingPoint(InstrDesc.getOpcode());
  }

  bool canProduceNaN(const MCInstrDesc &InstrDesc) const override {
    return snippy::canProduceNaN(InstrDesc);
  }

  // Get the most superregister and its class id. For example, for F1_H, the
  // pair <F1_D, RISCV::FPR64RegClassID> will be returned, if +d extension is
  // enabled, otherwise it will return <F1_F, RISCV::FPR32RegClassID>
  std::optional<std::pair<MCRegister, unsigned>>
  getMostSuperFPRegWithClassID(const InstructionGenerationContext &InstrGenCtx,
                               const MCRegisterInfo &RI, MCRegister Reg) const {
    assert(isFloatingPoint(Reg));
    static constexpr std::array<unsigned, 3> FPRClassesID{
        RISCV::FPR16RegClassID, RISCV::FPR32RegClassID, RISCV::FPR64RegClassID};

    auto &ST = InstrGenCtx.getSubtarget<RISCVSubtarget>();
    SmallVector<MCRegister, 2> SupRegs(RI.superregs(Reg));
    // When any of the zfh, zfhmin, or f extensions are enabled, we can only
    // unNaN FPR32 registers. UnNaN operations on FPR64 registers require
    // instructions from the d extension, which is not present in these
    // configurations. Also handle the case when we have 32-bit GPR registers
    // and can only overwrite single precision FP registers.
    llvm::erase_if(SupRegs, [&](auto SupReg) {
      return RI.getRegClass(RISCV::FPR64RegClassID).contains(SupReg) &&
             (!ST.hasStdExtD() || ST.getXLen() == 32);
    });
    if (SupRegs.empty())
      return std::nullopt;
    // When the +d extension is enabled, the FPR64 register must be obtained. It
    // is a superregister of FPR32
    if (SupRegs.size() == 2)
      llvm::erase_if(SupRegs, [&](auto &&SupReg) {
        return !RI.getRegClass(RISCV::FPR64RegClassID).contains(SupReg);
      });
    assert(SupRegs.size() == 1);
    auto SuperReg = SupRegs.front();
    auto ClassIDIter = llvm::find_if(FPRClassesID, [&](auto ID) {
      return RI.getRegClass(ID).contains(SuperReg);
    });
    assert(ClassIDIter != FPRClassesID.end());
    return std::make_pair(SuperReg, *ClassIDIter);
  }

  // Returns an optional pair of the most super-register for the provided `Reg`
  // and its class. Returns nullopt if a super-register does not exist for the
  // `Reg`.
  std::optional<std::pair<MCRegister, const MCRegisterClass *>>
  tryGetNaNRegisterAndClass(InstructionGenerationContext &InstrGenCtx,
                            MCRegister Reg) const override {
    assert(isFloatingPoint(Reg));

    auto &ProgCtx = InstrGenCtx.ProgCtx;
    auto &RI = ProgCtx.getLLVMState().getRegInfo();
    // We only keeps the most superregister because there's no point in
    // flagging the single-precision register as NaN if we're going to
    // overwrite the double-precision register later.
    if (auto OptPair = getMostSuperFPRegWithClassID(InstrGenCtx, RI, Reg);
        OptPair.has_value()) {
      auto [SuperReg, RegClassID] = OptPair.value();
      // If we write to subreg then super register will become NaN
      return std::make_pair(SuperReg, &RI.getRegClass(RegClassID));
    }
    return std::nullopt;
  }

  std::unique_ptr<AsmPrinter>
  createAsmPrinter(TargetMachine &TM,
                   std::unique_ptr<MCStreamer> Streamer) const override {
    return std::make_unique<RISCVAsmPrinter>(TM, std::move(Streamer));
  }

  uint8_t getCodeAlignment(const TargetSubtargetInfo &STI) const override {
    const auto &ST = static_cast<const RISCVSubtarget &>(STI);
    if (ST.hasStdExtC())
      return 2;
    return 4;
  }

  MachineBasicBlock::iterator generateJump(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator Ins,
                                           MachineBasicBlock &TBB,
                                           LLVMState &State) const override {
    auto &InstrInfo = State.getInstrInfo();
    return *getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                                  InstrInfo.get(RISCV::PseudoBR))
                .addMBB(&TBB)
                .getInstr();
  }

  void addAsmPrinterFlags(MachineInstr &MI) const override {
    // Add DoNotCompress flags only to main instructions as they must correspond
    // to the given histogram. On the other hand, we'd like to compress support
    // instructions as much as possible to reduce total overhead.
    if (!checkMetadata(MI, SnippyMetadata::Support))
      MI.setAsmPrinterFlag(RISCV::DoNotCompress);
  }
};

static unsigned getOpcodeForGPRToFPRInstr(unsigned DstReg, unsigned XLen,
                                          unsigned NumBits, LLVMContext &Ctx) {
  if (NumBits > XLen)
    snippy::fatal(Ctx, "Cannot write value to a FP register",
                  "it doesn't fit in GRP. Please, provide '" +
                      InitFRegsFromMemory.ArgStr +
                      "' option to make "
                      "initialization possible");

  unsigned FMVOpc;
  if (RISCV::FPR32RegClass.contains(DstReg))
    FMVOpc = RISCV::FMV_W_X;
  else if (RISCV::FPR64RegClass.contains(DstReg))
    FMVOpc = RISCV::FMV_D_X;
  else if (RISCV::FPR16RegClass.contains(DstReg))
    FMVOpc = RISCV::FMV_H_X;
  else
    snippy::fatal("unknown floating point register class for the register");

  return FMVOpc;
}

void SnippyRISCVTarget::generateWriteValueFP(
    InstructionGenerationContext &IGC, APInt Value, unsigned DstReg,
    SmallVectorImpl<MCInst> &Insts) const {
  const auto &SimCtx = IGC.SimCtx;
  assert(RISCV::FPR64RegClass.contains(DstReg) ||
         RISCV::FPR32RegClass.contains(DstReg) ||
         RISCV::FPR16RegClass.contains(DstReg));
  auto NumBits = Value.getBitWidth();

  if (InitFRegsFromMemory) {
    auto &ProgCtx = IGC.ProgCtx;
    auto &GP = ProgCtx.getOrAddGlobalsPoolFor(
        IGC.getSnippyModule(),
        "Failed to allocate global constant for float register value load");
    auto *GV = GP.createGV(
        Value, /* Alignment */ NumBits / RISCV_CHAR_BIT,
        /* Linkage */ GlobalValue::InternalLinkage,
        /* Name */ "global",
        /* Reason */ "This is needed for updating of float register");

    auto GVAddr = GP.getGVAddress(GV);
    generateLoadRegFromAddr(IGC, GVAddr, DstReg, Insts);
    if (SimCtx.hasModel())
      SimCtx.notifyMemUpdate(GVAddr, Value);
    return;
  }

  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  auto FMVOpc =
      getOpcodeForGPRToFPRInstr(DstReg, ST.getXLen(), NumBits, State.getCtx());

  Value = Value.zext(ST.getXLen());
  Value.setBitsFrom(NumBits);

  auto &RI = State.getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto &RP = IGC.getRegPool();
  auto ScratchReg =
      getNonZeroReg("scratch register for writing FP register", RI, RegClass,
                    RP, IGC.MBB, AccessMaskBit::SRW);

  generateWriteValueSeq(IGC, Value, ScratchReg, Insts);

  Insts.emplace_back(MCInstBuilder(FMVOpc).addReg(DstReg).addReg(ScratchReg));
}

// [Unsafe] This function expects certain RVVMode (SEW == XLEN && VL --> max)
void SnippyRISCVTarget::rvvUnsafeWriteValueUsingXReg(
    InstructionGenerationContext &IGC, APInt Value, unsigned DstReg) const {
  LLVM_DEBUG(dbgs() << "Writing to V" << (DstReg - RISCV::V0)
                    << " with a use of a slide1down sequence\n");
  auto &Ins = IGC.Ins;
  auto &MBB = IGC.MBB;
  auto &RP = IGC.getRegPool();
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &InstrInfo = State.getInstrInfo();
  assert(ST.hasStdExtV());

  // Use non-reserved reg as scratch.
  auto &RI = State.getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto XScratchReg = getNonZeroReg("scratch register", RI, RegClass, RP,
                                   MBB, // change getNonZeroReg
                                   AccessMaskBit::SRW);

  assert(RISCV::VRRegClass.contains(DstReg));
  const auto SEW = static_cast<unsigned>(RGC.getSEW(MBB));
  const auto ELEN = ST.getELen();
  const auto VL = RGC.getVL(MBB);
  const auto VLEN = RGC.getVLEN();
  assert(SEW == ELEN && VL * SEW == VLEN);
  // FIXME: We must set undef flag only when we do initialization. In all
  // other cases it's not quite right to use it. However, I expect the whole
  // this function to be a temporary solution, so it shouldn't be a big
  // problem.
  unsigned RegFlags = RegState::Undef;
  for (unsigned Idx = 0; Idx < VL; ++Idx) {
    auto EltValue = Value.extractBitsAsZExtValue(SEW, Idx * SEW);
    writeValueToReg(IGC, APInt(SEW, EltValue), XScratchReg);
    getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                          InstrInfo.get(RISCV::VSLIDE1DOWN_VX), DstReg)
        .addReg(DstReg, RegFlags)
        .addReg(XScratchReg)
        // Disable masking
        .addReg(RISCV::NoRegister);
    RegFlags = 0;
  }
}

// Returns std::nullopt if no RVVMode was installed or if it didn't change
std::optional<RVVModeInfo>
SnippyRISCVTarget::rvvWriteValueUsingXRegAndGetOldMode(
    InstructionGenerationContext &IGC, APInt Value, unsigned DstReg) const {
  auto &ProgCtx = IGC.ProgCtx;
  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &MBB = IGC.MBB;
  const auto &State = ProgCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();

  const auto &RVVModeToRestore = RGC.getRVVModeIfActive(MBB);
  const auto VLEN = RGC.getVLEN();
  const auto &TmpRVVMode = getSEWXlenVLMaxSupportRVVMode(IGC, MBB, VLEN);

  bool DidRVVModeChange =
      generateRVVModeUpdateIfNeeded(IGC, InstrInfo, TmpRVVMode);
  rvvUnsafeWriteValueUsingXReg(IGC, Value, DstReg);

  if (DidRVVModeChange)
    return RVVModeToRestore; // will be std::nullopt if no RVVMode were
                             // active
  return std::nullopt;
}

void SnippyRISCVTarget::rvvWriteValueUsingXReg(
    InstructionGenerationContext &IGC, APInt Value, unsigned DstReg) const {

  auto &ProgCtx = IGC.ProgCtx;
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  const auto &State = ProgCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  assert(ST.hasStdExtV());

  const auto &RVVModeToRestore =
      rvvWriteValueUsingXRegAndGetOldMode(IGC, Value, DstReg);

  if (RVVModeToRestore.has_value())
    generateRVVModeUpdate(IGC, InstrInfo, RVVModeToRestore.value());
}

// In case if there is no active RVV mode, the support RVVMode will be installed
// (LMUL = 1, SEW = XLEN, VL = VLEN/SEW)
void SnippyRISCVTarget::rvvWriteValueToV0UsingVReg(
    InstructionGenerationContext &IGC, APInt Value) const {
  LLVM_DEBUG(dbgs() << "Writing to V0 with a use of a slide1down sequence "
                       "and another vreg\n");
  auto &Ins = IGC.Ins;
  auto &MBB = IGC.MBB;
  auto &RP = IGC.getRegPool();
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &RI = State.getRegInfo();
  const auto &RegClass = RI.getRegClass(RISCV::VRRegClassID);
  auto VScratchReg = RP.getAvailableRegister(
      "scratch register to store the mask", RI, RegClass, MBB,
      /*Filter*/ [](unsigned Reg) { return Reg == RISCV::V0; },
      AccessMaskBit::SRW);

  const auto &RVVModeToRestore =
      rvvWriteValueUsingXRegAndGetOldMode(IGC, Value, VScratchReg);

  // copy from scratch register to V0
  getSupportInstBuilder(*this, MBB, Ins, State.getCtx(),
                        InstrInfo.get(RISCV::VMV_V_V), RISCV::V0)
      .addReg(VScratchReg);

  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  // Vector Mask is just what's in V0
  RGC.updateActiveRVVModeVM(&MBB, Value);

  // Restore only config, not the mask (v0)
  if (RVVModeToRestore.has_value())
    generateVTypeChange(IGC, InstrInfo, *RVVModeToRestore);
}

void SnippyRISCVTarget::rvvWriteValueUsingLoad(
    InstructionGenerationContext &IGC, APInt Value, unsigned DstReg) const {
  LLVM_DEBUG(dbgs() << "Writing to V" << (DstReg - RISCV::V0)
                    << " with a use of load\n");
  auto &SimCtx = IGC.SimCtx;
  auto &ProgCtx = IGC.ProgCtx;
  auto &GP = ProgCtx.getOrAddGlobalsPoolFor(
      IGC.getSnippyModule(),
      "Failed to allocate global constant for RVV register value load");

  auto *GV =
      GP.createGV(Value, /* Alignment */ Reg16Bytes,
                  /* Linkage */ GlobalValue::InternalLinkage,
                  /* Name */ "global",
                  /* Reason */ "This is needed for updating of RVV register");

  auto GVAddr = GP.getGVAddress(GV);
  loadRegFromAddr(IGC, GVAddr, DstReg);
  if (SimCtx.hasModel())
    SimCtx.notifyMemUpdate(GVAddr, Value);

  if (DstReg == RISCV::V0) {
    auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
    // Vector Mask is just what's in V0
    RGC.updateActiveRVVModeVM(&IGC.MBB, Value);
  }
}

void SnippyRISCVTarget::rvvWriteValue(InstructionGenerationContext &IGC,
                                      APInt Value, unsigned DstReg) const {

  assert(IGC.getSubtarget<RISCVSubtarget>().hasStdExtV());

  auto RVVInitMode = RVVInitModeOpt.getValue();

  if (DstReg == RISCV::V0) {
    assert(!NoMaskModeForRVV && "V0 should not be used in no mask mode");
    switch (RVVInitMode) {
    case RVVInitMode::Loads:
    case RVVInitMode::Mixed:
      rvvWriteValueUsingLoad(IGC, Value, RISCV::V0);
      return;
    case RVVInitMode::Slides:
    case RVVInitMode::Splats:
      rvvWriteValueToV0UsingVReg(IGC, Value);
      return;
    }
    llvm_unreachable("Unknown RVVInitMode");
  }

  switch (RVVInitMode) {
  case RVVInitMode::Loads:
    rvvWriteValueUsingLoad(IGC, Value, DstReg);
    return;
  case RVVInitMode::Mixed:
  case RVVInitMode::Slides:
  case RVVInitMode::Splats:
    rvvWriteValueUsingXReg(IGC, Value, DstReg);
    return;
  }
  llvm_unreachable("Unknown RVVInitMode");
}

void SnippyRISCVTarget::updateRVVConfig(InstructionGenerationContext &IGC,
                                        const MachineInstr &MI) const {
  auto &Ins = IGC.Ins;
  if (MI.getNumDefs() == 0)
    return;
  if (!isRVV(MI.getDesc().getOpcode()))
    return;
  assert(Ins.isValid());
  // Ins may points to end().
  // To get changeable MBB, decrease Ins pos by one.
  // This is always valid, as we create at least one instruction MI.
  auto &MBB = *(std::prev(Ins))->getParent();
  auto &ProgCtx = IGC.ProgCtx;
  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  auto &State = ProgCtx.getLLVMState();
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
    generateV0MaskUpdate(IGC, NewVLVM.VM, InstrInfo);
  }
}

void SnippyRISCVTarget::instructionPostProcess(
    InstructionGenerationContext &IGC, MachineInstr &MI) const {
  updateRVVConfig(IGC, MI);
}

void SnippyRISCVTarget::rvvGenerateModeSwitchAndUpdateContext(
    const MCInstrInfo &InstrInfo, InstructionGenerationContext &IGC,
    std::optional<unsigned> DesiredOpcode) const {
  auto &MBB = IGC.MBB;
  auto &ProgCtx = IGC.ProgCtx;
  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &VUInfo = RGC.getVUConfigInfo();
  // TODO: likely, we should return a pair
  const auto &NewRvvCFG = VUInfo.selectConfiguration();
  const auto &ModeChangeInfo = RGC.getVUConfigInfo().getModeChangeInfo();
  // VSETIVLI supports only reduced VL
  const bool MustUseReducedVL =
      (DesiredOpcode == RISCV::VSETIVLI) ||
      (ModeChangeInfo.WeightVSETVL + ModeChangeInfo.WeightVSETVLI) <
          std::numeric_limits<double>::epsilon();

  auto NewVLVM = VUInfo.selectVLVM(NewRvvCFG, MustUseReducedVL);

  assert(!(MustUseReducedVL && NewVLVM.VL > kMaxVLForVSETIVLI) &&
         "VSETIVLI supports only VLs up to specified maximum");

  const RVVModeInfo NewRVVMode{NewVLVM, NewRvvCFG, MBB};
  generateRVVModeUpdate(IGC, InstrInfo, NewRVVMode, DesiredOpcode);
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
      snippy::fatal(formatv("The specified restrictions on VSET* instructions "
                            "do not allow to produce VL of {0}. Please, adjust "
                            "the histogram or change the set of "
                            "reachable RVV configurations",
                            VL));

    DiscreteGeneratorInfo<unsigned, std::array<unsigned, 3>> Gen(
        {RISCV::VSETVL, RISCV::VSETVLI, RISCV::VSETIVLI}, VsetvlProb);
    return Gen();
  }
  case RVVModeChangeMode::MC_VSETIVLI:
    if (VL > kMaxVLForVSETIVLI)
      snippy::fatal("cannot select VSETIVLI as mode changing instruction "
                    " for VL greater than 31");
    return RISCV::VSETIVLI;
  case RVVModeChangeMode::MC_VSETVLI:
    return RISCV::VSETVLI;
  case RVVModeChangeMode::MC_VSETVL:
    return RISCV::VSETVL;
  }
  llvm_unreachable("unexpected RVV mode change preference");
}

// This function might generate VSET using RVVMode.Config
// Therefore it must be a config that you have already
// selected or that you are about to select.
void SnippyRISCVTarget::generateV0MaskUpdate(
    InstructionGenerationContext &IGC, const APInt VM,
    const MCInstrInfo &InstrInfo) const {
  if (NoMaskModeForRVV)
    return;
  const auto &MBB = IGC.MBB;
  auto &RGC = IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();

  // In case of an illegal configuration, we cannot use generateRVVMaskReset
  // because it uses vmxnor.mm instruction that will throw an exception if vill
  // bit is set, so we use load
  assert(RGC.hasActiveRVVMode(MBB));
  if (VM.isAllOnes() && RGC.getCurrentRVVCfg(MBB).IsLegal) {
    LLVM_DEBUG(dbgs() << "Resetting mask instruction for mask:"
                      << toString(VM, /* Radix */ 16,
                                  /* Signed */ false)
                      << "\n");
    const auto &ActiveRVVMode = RGC.getActiveRVVMode(MBB);
    // Mask elements past VL - the tail elements, are always updated with a
    // tail-agnostic policy (VMXNOR_MM guaranties result only on first VL bits)
    assert(VM.getBitWidth() == ActiveRVVMode.VLVM.VL);
    generateRVVMaskReset(IGC, InstrInfo, *this);
    return;
  }

  auto VLEN = RGC.getVLEN();
  auto WidenVM = VM.zext(VLEN);
  LLVM_DEBUG(
      bool WillUseMemoryInstr =
          RVVInitModeOpt.getValue() == RVVInitMode::Mixed ||
          RVVInitModeOpt.getValue() == RVVInitMode::Loads;
      dbgs() << "Mask update using "
             << (WillUseMemoryInstr ? "memory instruction" : "slides sequence")
             << " for mask:"
             << toString(WidenVM, /* Radix */ 16, /* Signed */ false) << "\n");
  rvvWriteValue(IGC, WidenVM, RISCV::V0);
}

void SnippyRISCVTarget::generateVSETIVLI(InstructionGenerationContext &IGC,
                                         const MCInstrInfo &InstrInfo,
                                         unsigned VTYPE, unsigned VL,
                                         bool SupportMarker) const {
  auto &MBB = IGC.MBB;
  auto &Ins = IGC.Ins;
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &RI = State.getRegInfo();
  auto &RP = IGC.getRegPool();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto DstReg = RP.getAvailableRegister("VSETIVLI dst", RI, RegClass, MBB,
                                        SupportMarker ? AccessMaskBit::SRW
                                                      : AccessMaskBit::GRW);
  // TODO: eventually this should be an assert
  if (VL > kMaxVLForVSETIVLI)
    snippy::fatal(formatv("cannot set the desired VL {0} since selected "
                          "VSETIVLI does not support it",
                          VL));
  auto MIB = getInstBuilder(SupportMarker, *this, MBB, Ins,
                            ProgCtx.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETIVLI));
  MIB.addDef(DstReg).addImm(VL).addImm(VTYPE);
}

void SnippyRISCVTarget::generateVSETVLI(InstructionGenerationContext &IGC,
                                        const MCInstrInfo &InstrInfo,
                                        unsigned VTYPE, unsigned VL,
                                        bool SupportMarker) const {
  auto &RP = IGC.getRegPool();
  auto &MBB = IGC.MBB;
  auto &Ins = IGC.Ins;
  auto &ProgCtx = IGC.ProgCtx;
  // TODO 1: if VL is equal to VLMAX we can use X0 if DstReg is not zero
  // TODO 2: if VL is not changed, and DST is zero, scratch VL can be zero
  const auto &RI = ProgCtx.getLLVMState().getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto DstReg = RP.getAvailableRegister("for VSETVLI dst", RI, RegClass, MBB,
                                        SupportMarker ? AccessMaskBit::SRW
                                                      : AccessMaskBit::GRW);
  auto ScratchRegVL = getNonZeroReg("for VSETVLI VL", RI, RegClass, RP, MBB,
                                    AccessMaskBit::SRW);
  writeValueToReg(IGC, APInt(IGC.getSubtarget<RISCVSubtarget>().getXLen(), VL),
                  ScratchRegVL);
  auto MIB = getInstBuilder(SupportMarker, *this, MBB, Ins,
                            ProgCtx.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETVLI));
  MIB.addDef(DstReg);
  MIB.addReg(ScratchRegVL);
  MIB.addImm(VTYPE);
}

void SnippyRISCVTarget::generateVSETVL(InstructionGenerationContext &IGC,
                                       const MCInstrInfo &InstrInfo,
                                       unsigned VTYPE, unsigned VL,
                                       bool SupportMarker) const {
  // TODO 1: if VL is equal to VLMAX we can use X0 if DstReg is not zero
  // TODO 2: if VL is not changed, and DST is zero, scratch VL can be zero
  auto &MBB = IGC.MBB;
  auto &Ins = IGC.Ins;
  auto &ProgCtx = IGC.ProgCtx;
  const auto &RI = ProgCtx.getLLVMState().getRegInfo();
  auto &RegClass = RI.getRegClass(RISCV::GPRRegClassID);
  auto RP = IGC.pushRegPool();
  auto DstReg = RP->getAvailableRegister("for VSETVL dst", RI, RegClass, MBB,
                                         SupportMarker ? AccessMaskBit::SRW
                                                       : AccessMaskBit::GRW);
  const auto &ST = IGC.getSubtarget<RISCVSubtarget>();
  // TODO: maybe just use GPRNoX0RegClassID class?
  auto [ScratchRegVL, ScratchRegVType] = RP->getNAvailableRegisters<2>(
      "registers for VSETVL VL and VType", RI, RegClass, MBB,
      /* Filter */ [](unsigned Reg) { return Reg == RISCV::X0; },
      AccessMaskBit::SRW);
  writeValueToReg(IGC, APInt(ST.getXLen(), VL), ScratchRegVL);
  RP->addReserved(ScratchRegVL);
  writeValueToReg(IGC, APInt(ST.getXLen(), VTYPE), ScratchRegVType);
  auto MIB = getInstBuilder(SupportMarker, *this, MBB, Ins,
                            ProgCtx.getLLVMState().getCtx(),
                            InstrInfo.get(RISCV::VSETVL));
  MIB.addDef(DstReg);
  MIB.addReg(ScratchRegVL);
  MIB.addReg(ScratchRegVType);
}

// generates VSET{I}VL{I} without V0 update
void SnippyRISCVTarget::generateVTypeChange(
    InstructionGenerationContext &IGC, const MCInstrInfo &InstrInfo,
    const RVVModeInfo &NewRVVMode,
    std::optional<unsigned> DesiredOpcode) const {
  assert(NewRVVMode.Config != nullptr);
  const auto VL = NewRVVMode.VLVM.VL;
  const auto &Config = *NewRVVMode.Config;
  unsigned SEW = static_cast<unsigned>(Config.SEW);
  LLVM_DEBUG(dbgs() << "Emit RVV Mode Change: VL = " << VL << ", SEW = " << SEW
                    << ", TA = " << Config.TailAgnostic
                    << ", MA = " << Config.MaskAgnostic << "\n");
  auto VTYPE = RISCVVType::encodeVTYPE(Config.LMUL, SEW, Config.TailAgnostic,
                                       Config.MaskAgnostic);
  auto &ProgCtx = IGC.ProgCtx;
  auto &RGC = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  bool SupportMarker = false;
  if (!DesiredOpcode.has_value()) {
    DesiredOpcode =
        selectDesiredModeChangeInstruction(RVVModeChangePreferenceOpt, VL, RGC);
    SupportMarker = true;
  } else {
    const auto &VUInfo = RGC.getVUConfigInfo();
    SupportMarker = VUInfo.isModeChangeArtificial();
  }

  switch (DesiredOpcode.value()) {
  case RISCV::VSETIVLI:
    generateVSETIVLI(IGC, InstrInfo, VTYPE, VL, SupportMarker);
    break;
  case RISCV::VSETVLI:
    generateVSETVLI(IGC, InstrInfo, VTYPE, VL, SupportMarker);
    break;
  case RISCV::VSETVL:
    generateVSETVL(IGC, InstrInfo, VTYPE, VL, SupportMarker);
    break;
  default:
    llvm_unreachable("unexpected OpcodeRequested for generateVTypeChange");
  }
  RGC.updateActiveRVVModeConfigAndVL(&IGC.MBB, NewRVVMode.Config, VL);
}

void SnippyRISCVTarget::generateRVVModeUpdate(
    InstructionGenerationContext &IGC, const MCInstrInfo &InstrInfo,
    const RVVModeInfo &NewRVVMode,
    std::optional<unsigned> DesiredOpcode) const {

  generateVTypeChange(IGC, InstrInfo, NewRVVMode, DesiredOpcode);
  generateV0MaskUpdate(IGC, NewRVVMode.VLVM.VM, InstrInfo);

  // Check that we got what expected, even if there was a change to temporary
  // RVV mode required for the mask update
  const auto &RGC =
      IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto ActiveRVVMode = RGC.getActiveRVVMode(IGC.MBB);

  assert(APInt::isSameValue(ActiveRVVMode.VLVM.VM, NewRVVMode.VLVM.VM) ||
         NoMaskModeForRVV);
  assert(ActiveRVVMode.VLVM.VL == NewRVVMode.VLVM.VL);
  assert(*ActiveRVVMode.Config == *NewRVVMode.Config);
  assert(ActiveRVVMode.MBBGuard == NewRVVMode.MBBGuard);

  // TODO: update VXRM
}

// return false if no update were needed
bool SnippyRISCVTarget::generateRVVModeUpdateIfNeeded(
    InstructionGenerationContext &IGC, const MCInstrInfo &InstrInfo,
    const RVVModeInfo &NewRVVMode,
    std::optional<unsigned> DesiredOpcode) const {
  const auto &RGC =
      IGC.ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  const auto &MBB = IGC.MBB;
  if (RGC.hasActiveRVVMode(MBB) && RGC.getActiveRVVMode(MBB) == NewRVVMode)
    return false;
  generateRVVModeUpdate(IGC, InstrInfo, NewRVVMode, DesiredOpcode);
  return true;
}

static void dumpRvvConfigurationInfo(StringRef FilePath,
                                     const RVVConfigurationInfo &RVVCfg) {
  if (FilePath.empty()) {
    RVVCfg.print(outs());
    return;
  }

  auto ReportFileError = [FilePath](const std::error_code &EC) {
    snippy::fatal(formatv("could not create {0} for RVV config dump: {1}",
                          FilePath, EC.message()));
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
SnippyRISCVTarget::createTargetContext(LLVMState &State, const Config &Cfg,
                                       const TargetSubtargetInfo *STI) const {
  auto RISCVCfg = RISCVConfigurationInfo::constructConfiguration(State, Cfg);
  auto RGC = std::make_unique<RISCVGeneratorContext>(std::move(RISCVCfg));
  const auto &VUInfo = RGC->getVUConfigInfo();
  bool IsRVVPresent = VUInfo.getModeChangeInfo().RVVPresent;
  if (IsRVVPresent) {
    // TODO: This should be checked in some other way.
    bool IsApplyValuegramEachInst = Cfg.DefFlowConfig.Valuegram.has_value();
    if (IsApplyValuegramEachInst)
      snippy::fatal("Not implemented", "vector registers can't be initialized");
  }

  if (DumpRVVConfigurationInfo.isSpecified())
    dumpRvvConfigurationInfo(DumpRVVConfigurationInfo.getValue(), VUInfo);

  return std::move(RGC);
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

  auto MisalignedAccessMode = getMisalignedAccessMode();
  return createRISCVSimulator(
      ModelLib, Cfg, CallbackHandler, Subtarget, VLENB,
      !(MisalignedAccessMode == DisableMisalignedAccessMode::All));
}

const MCRegisterClass &SnippyRISCVTarget::getRegClass(
    InstructionGenerationContext &IGC, unsigned OperandRegClassID,
    unsigned OpIndex, unsigned Opcode, const MCRegisterInfo &RegInfo) const {
  auto &MBB = IGC.MBB;
  auto &ProgCtx = IGC.ProgCtx;
  auto &TgtCtx = ProgCtx.getTargetContext().getImpl<RISCVGeneratorContext>();
  if (!isRVV(Opcode) || !TgtCtx.hasActiveRVVMode(MBB) ||
      (OperandRegClassID != RISCV::VRRegClassID))
    return RegInfo.getRegClass(OperandRegClassID);

  auto [Multiplier, IsFractional] = TgtCtx.getEMUL(Opcode, OpIndex, MBB);
  // Special handling for Vector Load/Store Segment Instructions, because
  // this instructions moves subarrays.
  if ((isRVVUnitStrideSegLoadStore(Opcode) ||
       isRVVStridedSegLoadStore(Opcode) || isRVVIndexedSegLoadStore(Opcode)) &&
      OpIndex == 0 /* vector destination register group */)
    return getRVVSegLoadStoreRegClassForVd(
        Opcode, std::pair(Multiplier, IsFractional), RegInfo);

  // FIXME: The vector integer extension instructions (zero- or sign-extend)
  // also need special handling. The EEW of the source is 1/2, 1/4, or 1/8 of
  // SEW => EMUL is 1/2, 1/4, or 1/8 of LMUL. The destination has EEW equal to
  // SEW and EMUL equal to LMUL. Now, it's believed that EMUL_source ==
  // EMUL_destination == LMUL. That means we're missing a lot of source
  // registers. For example, we'll never generate with LMUL=8, vzext.vf4 v0, v6
  // (now, we can only use registers that divide by 8), even though we should.
  // But the thing to keep in mind here is that overlaps are only allowed in the
  // highest-numbered part of the destination register group (a source of v0,
  // v2, or v4 is not allowed). (riscv-v-spec-1.0, "Vector Operands" section)
  if (IsFractional)
    return RegInfo.getRegClass(OperandRegClassID);
  switch (Multiplier) {
  case 2:
    return RegInfo.getRegClass(RISCV::VRM2RegClassID);
  case 4:
    return RegInfo.getRegClass(RISCV::VRM4RegClassID);
  case 8:
    return RegInfo.getRegClass(RISCV::VRM8RegClassID);
  default:
    return RegInfo.getRegClass(OperandRegClassID);
  }
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
