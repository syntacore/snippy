//==- AArch64AsmGenerator.h - Generate AArch64 operands for MCInst ---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "Utils/AArch64BaseInfo.h"
#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SnippyOperandGenerator.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCAsmParserExtension.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sys/types.h>
#include <vector>

// FIX: experimental AArch64 support, remove after bringup
#define SNIPPY_UNIMPLEMENTED()                                                 \
  snippy::fatal(llvm::Twine("AArch64 experimental: ") + __PRETTY_FUNCTION__ +  \
                " at " + __FILE__ + ":" + llvm::Twine(__LINE__))

namespace llvm {

namespace snippy {
// TODO: rework this class
enum class RegKind {
  Scalar,
  NeonVector,
  SVEDataVector,
  SVEPredicateAsCounter,
  SVEPredicateVector,
  Matrix,
  LookupTable
};
namespace AArch64Operand {
enum VecListIndexType {
  VecListIdx_DReg = 0,
  VecListIdx_QReg = 1,
  VecListIdx_ZReg = 2,
  VecListIdx_PReg = 3,
};
} // namespace AArch64Operand

enum class MatrixKind { Array, Tile, Row, Col };

enum RegConstraintEqualityTy { EqualsReg, EqualsSuperReg, EqualsSubReg };

/// AArch64OpndGenerator - Instances of this class represent a parsed AArch64
/// machine instruction.
class AArch64OpndGenerator final : public SnippyOperandGenerator {
private:
  struct TokOp {
    const char *Data;
    unsigned Length;
    bool IsSuffix; // Is the operand actually a suffix on the mnemonic.
  };

  // Separate shift/extend operand.
  struct ShiftExtendOp {
    AArch64_AM::ShiftExtendType Type;
    unsigned Amount;
    bool HasExplicitAmount;
  };

  struct RegOp {
    unsigned RegNum;
    RegKind Kind;
    int ElementWidth;

    // The register may be allowed as a different register class,
    // e.g. for GPR64as32 or GPR32as64.
    RegConstraintEqualityTy EqualityTy;

    // In some cases the shift/extend needs to be explicitly parsed together
    // with the register, rather than as a separate operand. This is needed
    // for addressing modes where the instruction as a whole dictates the
    // scaling/extend, rather than specific bits in the instruction.
    // By parsing them as a single operand, we avoid the need to pass an
    // extra operand in all CodeGen patterns (because all operands need to
    // have an associated value), and we avoid the need to update TableGen to
    // accept operands that have no associated bits in the instruction.
    //
    // An added benefit of parsing them together is that the assembler
    // can give a sensible diagnostic if the scaling is not correct.
    //
    // The default is 'lsl #0' (HasExplicitAmount = false) if no
    // ShiftExtend is specified.
    ShiftExtendOp ShiftExtend;
  };

  struct MatrixRegOp {
    unsigned RegNum;
    unsigned ElementWidth;
    MatrixKind Kind;
  };

  struct MatrixTileListOp {
    unsigned RegMask = 0;
  };

  struct VectorListOp {
    unsigned RegNum;
    unsigned Count;
    unsigned Stride;
    unsigned NumElements;
    unsigned ElementWidth;
    RegKind RegisterKind;
  };

  struct VectorIndexOp {
    int Val;
  };

  struct ImmOp {
    const MCExpr *Val;
  };

  struct ShiftedImmOp {
    uint64_t Val;
    unsigned ShiftAmount;
  };

  struct ImmRangeOp {
    unsigned First;
    unsigned Last;
  };

  struct CondCodeOp {
    AArch64CC::CondCode Code;
  };

  struct FPImmOp {
    uint64_t Val; // APFloat value bitcasted to uint64_t.
    bool IsExact; // describes whether parsed value was exact.
  };

  struct BarrierOp {
    const char *Data;
    unsigned Length;
    unsigned Val; // Not the enum since not all values have names.
    bool HasnXSModifier;
  };

  struct SysRegOp {
    const char *Data;
    unsigned Length;
    uint32_t MRSReg;
    uint32_t MSRReg;
    uint32_t PStateField;
  };

  struct SysCRImmOp {
    unsigned Val;
  };

  struct PrefetchOp {
    const char *Data;
    unsigned Length;
    unsigned Val;
  };

  struct PSBHintOp {
    const char *Data;
    unsigned Length;
    unsigned Val;
  };
  struct PHintOp {
    const char *Data;
    unsigned Length;
    unsigned Val;
  };
  struct BTIHintOp {
    const char *Data;
    unsigned Length;
    unsigned Val;
  };

  struct SVCROp {
    const char *Data;
    unsigned Length;
    unsigned PStateField;
  };

  enum class ShiftImmKind { Exact, Not, MSL, MSL_Not };

public:
  AArch64OpndGenerator(InstructionGenerationContext &InstrGenCtx,
                       const MCInstrDesc &InstrDesc)
      : SnippyOperandGenerator(InstrGenCtx, InstrDesc) {}

  unsigned getFirstImmVal() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getLastImmVal() { SNIPPY_UNIMPLEMENTED(); }

  AArch64CC::CondCode getCondCode() { SNIPPY_UNIMPLEMENTED(); }

  APFloat getFPImm() { SNIPPY_UNIMPLEMENTED(); }

  bool getFPImmIsExact() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getBarrier() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getBarrierName() { SNIPPY_UNIMPLEMENTED(); }

  bool getBarriernXSModifier() { SNIPPY_UNIMPLEMENTED(); }

  Register getReg(const planning::PreselectedOpInfo &Selected) {
    // TODO: Check that we can get current operand using generated one
    const MCOperandInfo &MCOpInfo = InstrDesc.operands()[Operands.size()];
    assert(MCOpInfo.RegClass != -1);
    auto OperandRegClassID = MCOpInfo.RegClass;
    auto &RP = InstrGenCtx.getRegPool();
    auto &MBB = InstrGenCtx.MBB;
    auto &ProgCtx = InstrGenCtx.ProgCtx;
    auto &RegGen = ProgCtx.getRegGen();
    const auto &State = ProgCtx.getLLVMState();
    const auto &RegInfo = State.getRegInfo();
    const auto &SnippyTgt = State.getSnippyTarget();
    bool IsDst = Selected.getFlags() & RegState::Define;
    AccessMaskBit Mask = IsDst ? AccessMaskBit::W : AccessMaskBit::R;
    if (Selected.isTiedTo()) {
      return Operands[Selected.getTiedTo()].getReg();
    }
    auto RegClass = RegInfo.getRegClass(OperandRegClassID);
    auto ExpectedReg =
        RegGen.generate(RegClass, OperandRegClassID, RegInfo, RP, MBB,
                        SnippyTgt, /* Exclude */ {}, /* Include */ {}, Mask);
    if (auto Err = ExpectedReg.takeError())
      snippy::fatal(std::move(Err));
    return *ExpectedReg;
  }

  unsigned getMatrixReg() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getMatrixElementWidth() { SNIPPY_UNIMPLEMENTED(); }

  MatrixKind getMatrixKind() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getMatrixTileListRegMask() { SNIPPY_UNIMPLEMENTED(); }

  RegConstraintEqualityTy getRegEqualityTy() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getVectorListStart() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getVectorListCount() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getVectorListStride() { SNIPPY_UNIMPLEMENTED(); }

  int getVectorIndex() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getSysReg() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getSysCR() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getPrefetch() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getPSBHint() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getPHint() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getPSBHintName() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getPHintName() { SNIPPY_UNIMPLEMENTED(); }

  unsigned getBTIHint() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getBTIHintName() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getSVCR() { SNIPPY_UNIMPLEMENTED(); }

  StringRef getPrefetchName() { SNIPPY_UNIMPLEMENTED(); }

  AArch64_AM::ShiftExtendType getShiftExtendType() {
    // SNIPPY_UNIMPLEMENTED();

    return static_cast<AArch64_AM::ShiftExtendType>(
        RandEngine::genInRangeInclusive<int>(AArch64_AM::ShiftExtendType::LSL,
                                             AArch64_AM::ShiftExtendType::MSL));
  }

  unsigned getShiftExtendAmount() {
    return RandEngine::genInRangeInclusive<int>(0, 8);
  }

  bool hasShiftExtendAmount() { SNIPPY_UNIMPLEMENTED(); }

  // Add Operands methods
  void addRegOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    assert(Preselected.size() == 1);
    Operands.push_back(
        createRegAsOperand(getReg(Preselected[0]), Preselected[0].getFlags()));
    ;
  }

  void addShiftedImmOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    [[maybe_unused]] unsigned ValueWidth = 0;
    unsigned ImmWidth = 0;
    llvm::SmallVector<unsigned, 4> Shifts;
    [[maybe_unused]] ShiftImmKind Kind = ShiftImmKind::Exact;
    switch (InstrDesc.getOpcode()) {
    // W: imm16, shifts {0,16}
    case AArch64::MOVZWi:
    case AArch64::MOVNWi:
    case AArch64::MOVKWi:
      ValueWidth = 32;
      ImmWidth = 16;
      Shifts = {0u, 16u};
      Kind = ShiftImmKind::Exact;
      break;

    // X: imm16, shifts {0,16,32,48}
    case AArch64::MOVZXi:
    case AArch64::MOVNXi:
    case AArch64::MOVKXi:
      ValueWidth = 64;
      ImmWidth = 16;
      Shifts = {0u, 16u, 32u, 48u};
      Kind = ShiftImmKind::Exact;
      break;

    // AdvSIMD v*i32: imm8, shifts {0,8,16,24}
    case AArch64::MOVIv2i32:
    case AArch64::MOVIv4i32:
    case AArch64::ORRv2i32:
    case AArch64::ORRv4i32:
    case AArch64::BICv2i32:
    case AArch64::BICv4i32:
      ValueWidth = 32;
      ImmWidth = 8;
      Shifts = {0u, 8u, 16u, 24u};
      Kind = ShiftImmKind::Exact;
      break;

    // AdvSIMD v*i32 : NOT(imm<<shift)
    case AArch64::MVNIv2i32:
    case AArch64::MVNIv4i32:
      ValueWidth = 32;
      ImmWidth = 8;
      Shifts = {0u, 8u, 16u, 24u};
      Kind = ShiftImmKind::Not;
      break;

    // AdvSIMD v*i16: imm8, shifts {0,8}
    case AArch64::MOVIv4i16:
    case AArch64::MOVIv8i16:
    case AArch64::ORRv4i16:
    case AArch64::ORRv8i16:
    case AArch64::BICv4i16:
    case AArch64::BICv8i16:
      ValueWidth = 16;
      ImmWidth = 8;
      Shifts = {0u, 8u};
      Kind = ShiftImmKind::Exact;
      break;

    // AdvSIMD v*i16: NOT(imm<<shift)
    case AArch64::MVNIv4i16:
    case AArch64::MVNIv8i16:
      ValueWidth = 16;
      ImmWidth = 8;
      Shifts = {0u, 8u};
      Kind = ShiftImmKind::Not;
      break;

    // AdvSIMD *_msl: imm8, shifts {8,16}
    case AArch64::MOVIv2s_msl:
    case AArch64::MOVIv4s_msl:
      ValueWidth = 32;
      ImmWidth = 8;
      Shifts = {8u, 16u};
      Kind = ShiftImmKind::MSL;
      break;

    case AArch64::MVNIv2s_msl:
    case AArch64::MVNIv4s_msl:
      ValueWidth = 32;
      ImmWidth = 8;
      Shifts = {8u, 16u};
      Kind = ShiftImmKind::MSL_Not;
      break;

    default:
      snippy::fatal("Unsupported instruction for Shifted Immediate");
    }
    auto [Imm, Shift] = getShiftedImm<uint64_t>(ImmWidth, Shifts);
    Operands.push_back(MachineOperand::CreateImm(Imm));
    Operands.push_back(MachineOperand::CreateImm(Shift));
  }

  template <typename T>
  static inline std::pair<T, unsigned>
  getShiftedImmUniformly(unsigned ImmWidth, SmallVector<unsigned, 4> Shifts) {
    return {RandEngine::genInRangeInclusive((1ull << ImmWidth) - 1u),
            RandEngine::selectFromContainer(Shifts)};
  }
  template <typename T>
  std::pair<T, unsigned> getShiftedImm(unsigned ImmWidth,
                                       SmallVector<unsigned, 4> Shifts) {
    const CommonPolicyConfig &Cfg = InstrGenCtx.getCommonCfg();
    const auto &IHV = Cfg.ImmHistogram;
    const ImmediateHistogramSequence *Seq;
    if (IHV.holdsAlternative<ImmediateHistogramRegEx>()) {
      const auto &OpcSettings =
          Cfg.ImmHistMap.getConfigForOpcode(InstrDesc.getOpcode());
      if (OpcSettings.isUniform())
        return getShiftedImmUniformly<T>(ImmWidth, Shifts);
      Seq = &OpcSettings.getSequence();
    } else {
      Seq = &IHV.get<ImmediateHistogramSequence>();
    }
    if (Seq->empty())
      return getShiftedImmUniformly<T>(ImmWidth, Shifts);
    // If sequence not empty: filter out not fitting values
    assert(Seq->Values.size() == Seq->Weights.size());
    ImmediateHistogramSequence Suitable;
    Suitable.Values.reserve(Seq->Values.size());
    Suitable.Weights.reserve(Seq->Weights.size());
    uint64_t Val, Weight;
    auto Pred = [Imm = std::ref(Val), ImmWidth](unsigned Shift) {
      if (Imm & ((1ull << Shift) - 1u))
        return false;

      uint64_t FillValue = Imm >> Shift;
      uint64_t MaxFill = (1ull << ImmWidth) - 1u;

      return FillValue <= MaxFill;
    };

    for (auto Vals : llvm::zip_equal(Seq->Values, Seq->Weights)) {
      // FIXME: C++17 workaround, because structured bindings could be captured
      // only in C++20
      std::tie(Val, Weight) = Vals;
      if (llvm::any_of(Shifts, Pred)) {
        Suitable.Values.push_back(Val);
        Suitable.Weights.push_back(Weight);
      }
    }
    if (Suitable.empty())
      snippy::fatal("No suitable immediates in histogram");
    Val = genImmInInterval(Suitable, std::numeric_limits<int>::min(),
                           std::numeric_limits<int>::max());
    unsigned Shift = *llvm::find_if(Shifts, Pred);
    return {Val >> Shift, Shift};
  }
  void
  addMatrixOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(
        createRegAsOperand(getMatrixReg(), Preselected[0].getFlags()));
    ;
  }

  void addGPR32as64Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    unsigned R = getReg(Preselected[0]);
    // NOTE: Use X28 here, because FP and LR are aliases
    if (R >= AArch64::X0 && R <= AArch64::X28)
      R = AArch64::W0 + (R - AArch64::X0);
    Operands.push_back(createRegAsOperand(R, Preselected[0].getFlags()));
    ;
  }

  void addGPR64as32Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    unsigned R = getReg(Preselected[0]);
    if (R >= AArch64::W0 && R <= AArch64::W30)
      R = AArch64::X0 + (R - AArch64::W0);
    Operands.push_back(createRegAsOperand(R, Preselected[0].getFlags()));
    ;
  }

  template <int Width>
  void addFPRasZPRRegOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    unsigned Base = (Width == 8)    ? AArch64::B0
                    : (Width == 16) ? AArch64::H0
                    : (Width == 32) ? AArch64::S0
                    : (Width == 64) ? AArch64::D0
                                    :
                                    /*128*/ AArch64::Q0;
    Operands.push_back(
        createRegAsOperand(AArch64::Z0 + getReg(Preselected[0]) - Base,
                           Preselected[0].getFlags()));
    ;
  }

  void addPPRorPNRRegOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addPNRasPPRRegOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addVectorReg64Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(
        createRegAsOperand(getReg(Preselected[0]), Preselected[0].getFlags()));
  }

  void addVectorReg128Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addVectorRegLoOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addVectorReg0to7Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  template <AArch64Operand::VecListIndexType RegTy, unsigned NumRegs,
            bool IsConsecutive = false> // TODO: check it
  void addVectorListOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {

    SNIPPY_UNIMPLEMENTED();
  }
  template <unsigned NumRegs>
  void addStridedVectorListOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addMatrixTileListOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getMatrixTileListRegMask()));
  }

  void addVectorIndexOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getVectorIndex()));
  }

  template <unsigned ImmIs0, unsigned ImmIs1>
  void addExactFPImmOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addImmOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(
        getImmediate<0, std::numeric_limits<int>::max()>(Preselected[0])));
  }

  template <int ShiftWidth>
  void addImmWithOptionalShiftOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    auto [ImmVal, Shift] = getShiftedImm<int64_t>(ShiftWidth, {0, ShiftWidth});
    Operands.push_back(MachineOperand::CreateImm(ImmVal));
    Operands.push_back(MachineOperand::CreateImm(
        AArch64_AM::getShifterImm(AArch64_AM::LSL, Shift)));
  }

  template <int ShiftWidth>
  void addImmNegWithOptionalShiftOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    const auto [Imm, Shift] =
        getShiftedImm<int64_t>(ShiftWidth, {0, ShiftWidth});
    Operands.push_back(MachineOperand::CreateImm(-Imm));
    Operands.push_back(MachineOperand::CreateImm(Shift));
  }

  void
  addCondCodeOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getCondCode()));
  }

  void addAdrpLabelOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addAdrLabelOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
  }

  template <int Scale>
  void addUImm12OffsetOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addUImm6Operands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  template <int Scale>
  void addImmScaledOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  template <int Scale>
  void addImmScaledRangeOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getFirstImmVal() / Scale));
  }

  template <typename T>
  void addLogicalImmOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    const std::make_unsigned_t<T> V =
        getImmediate<0, std::numeric_limits<T>::max()>(Preselected[0]);
    uint64_t Enc = AArch64_AM::encodeLogicalImmediate(V, sizeof(T) * CHAR_BIT);
    Operands.push_back(MachineOperand::CreateImm(Enc));
  }

  template <typename T>
  void addLogicalImmNotOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    const std::make_unsigned_t<T> V = ~static_cast<std::make_unsigned_t<T>>(
        getImmediate<0, std::numeric_limits<T>::max()>(Preselected[0]));
    uint64_t Enc = AArch64_AM::encodeLogicalImmediate(V, sizeof(T) * CHAR_BIT);
    Operands.push_back(MachineOperand::CreateImm(Enc));
  }

  void addSIMDImmType10Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addBranchTarget26Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addPAuthPCRelLabel16Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addPCRelLabel19Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addPCRelLabel9Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addBranchTarget14Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addFPImmOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(
        AArch64_AM::getFP64Imm(getFPImm().bitcastToAPInt())));
  }

  void
  addBarrierOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getBarrier()));
  }

  void addBarriernXSOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addMRSSystemRegisterOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addMSRSystemRegisterOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addSystemPStateFieldWithImm0_1Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addSVCROperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addSystemPStateFieldWithImm0_15Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addSysCROperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getSysCR()));
  }

  void
  addPrefetchOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getPrefetch()));
  }

  void
  addPSBHintOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getPSBHint()));
  }

  void
  addPHintOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getPHint()));
  }

  void
  addBTIHintOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getBTIHint()));
  }

  void
  addShifterOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    unsigned Imm =
        AArch64_AM::getShifterImm(getShiftExtendType(), getShiftExtendAmount());
    Operands.push_back(MachineOperand::CreateImm(Imm));
  }

  void addShiftedRegOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addExtendedRegOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }
  void addLSLImm3ShifterOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    Operands.push_back(MachineOperand::CreateImm(getShiftExtendAmount()));
  }

  void addSyspXzrPairOperand(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addExtendOperands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void
  addExtend64Operands(llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
  }

  void addMemExtendOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addMemExtend8Operands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  template <int Shift>
  void addMOVZMovAliasOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  template <int Shift>
  void addMOVNMovAliasOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addComplexRotationEvenOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    SNIPPY_UNIMPLEMENTED();
  }

  void addComplexRotationOddOperands(
      llvm::ArrayRef<planning::PreselectedOpInfo> Preselected) {
    // TODO: fix it
    SNIPPY_UNIMPLEMENTED();
  }
#include "AArch64Operands.inc"
};

} // namespace snippy
} // namespace llvm
