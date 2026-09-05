//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for RISCV/Target.cpp.
///
//===----------------------------------------------------------------------===//

#include "RISCVSubtarget.h"

#include "snippy/unittests/SnippyState.h"

#include "gtest/gtest.h"

namespace llvm::snippy {
using namespace llvm::RISCV;

// Reg to storage
struct RISCVRegToStorage : public testing::SnippyState {
  RISCVRegToStorage() : SnippyState("riscv32", "", "", "+f,+d,+v") {}
};

TEST_F(RISCVRegToStorage, ArchitecturalRegisterIndices) {
  const auto &Target = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  for (const auto *RC : {&GPRRegClass, &FPR16RegClass, &FPR32RegClass,
                         &FPR64RegClass, &VRRegClass})
    for (auto Reg : *RC)
      EXPECT_EQ(Target.regToIndex(Reg), RI.getEncodingValue(Reg))
          << RI.getName(Reg);
  for (unsigned I = 0; I < 32; ++I) {
    EXPECT_EQ(Target.regToIndex(X0_H + I), I);
    EXPECT_EQ(Target.regToIndex(X0_W + I), I);
  }
}

#define CHECK_REG_STORAGE(Reg, Storage)                                        \
  EXPECT_EQ(SnippyTarget.regToStorage(Reg), RegStorageType::Storage)
#define CHECK_X_STORAGE(Reg) CHECK_REG_STORAGE(Reg, XReg)
#define CHECK_F_STORAGE(Reg) CHECK_REG_STORAGE(Reg, FReg)
#define CHECK_V_STORAGE(Reg) CHECK_REG_STORAGE(Reg, VReg)

TEST_F(RISCVRegToStorage, RISCVOpcode) {
  const auto &SnippyTarget = State.getSnippyTarget();

  CHECK_X_STORAGE(X0);
  CHECK_X_STORAGE(X1);
  CHECK_X_STORAGE(X2);
  CHECK_X_STORAGE(X3);
  CHECK_X_STORAGE(X4);
  CHECK_X_STORAGE(X5);
  CHECK_X_STORAGE(X6);
  CHECK_X_STORAGE(X7);
  CHECK_X_STORAGE(X8);
  CHECK_X_STORAGE(X9);
  CHECK_X_STORAGE(X10);
  CHECK_X_STORAGE(X11);
  CHECK_X_STORAGE(X12);
  CHECK_X_STORAGE(X13);
  CHECK_X_STORAGE(X14);
  CHECK_X_STORAGE(X15);
  CHECK_X_STORAGE(X16);
  CHECK_X_STORAGE(X17);
  CHECK_X_STORAGE(X18);
  CHECK_X_STORAGE(X19);
  CHECK_X_STORAGE(X20);
  CHECK_X_STORAGE(X21);
  CHECK_X_STORAGE(X22);
  CHECK_X_STORAGE(X23);
  CHECK_X_STORAGE(X24);
  CHECK_X_STORAGE(X25);
  CHECK_X_STORAGE(X26);
  CHECK_X_STORAGE(X27);
  CHECK_X_STORAGE(X28);
  CHECK_X_STORAGE(X29);
  CHECK_X_STORAGE(X30);
  CHECK_X_STORAGE(X31);
  CHECK_F_STORAGE(F0_F);
  CHECK_F_STORAGE(F1_F);
  CHECK_F_STORAGE(F2_F);
  CHECK_F_STORAGE(F3_F);
  CHECK_F_STORAGE(F4_F);
  CHECK_F_STORAGE(F5_F);
  CHECK_F_STORAGE(F6_F);
  CHECK_F_STORAGE(F7_F);
  CHECK_F_STORAGE(F8_F);
  CHECK_F_STORAGE(F9_F);
  CHECK_F_STORAGE(F10_F);
  CHECK_F_STORAGE(F11_F);
  CHECK_F_STORAGE(F12_F);
  CHECK_F_STORAGE(F13_F);
  CHECK_F_STORAGE(F14_F);
  CHECK_F_STORAGE(F15_F);
  CHECK_F_STORAGE(F16_F);
  CHECK_F_STORAGE(F17_F);
  CHECK_F_STORAGE(F18_F);
  CHECK_F_STORAGE(F19_F);
  CHECK_F_STORAGE(F20_F);
  CHECK_F_STORAGE(F21_F);
  CHECK_F_STORAGE(F22_F);
  CHECK_F_STORAGE(F23_F);
  CHECK_F_STORAGE(F24_F);
  CHECK_F_STORAGE(F25_F);
  CHECK_F_STORAGE(F26_F);
  CHECK_F_STORAGE(F27_F);
  CHECK_F_STORAGE(F28_F);
  CHECK_F_STORAGE(F29_F);
  CHECK_F_STORAGE(F30_F);
  CHECK_F_STORAGE(F31_F);
  CHECK_F_STORAGE(F0_D);
  CHECK_F_STORAGE(F1_D);
  CHECK_F_STORAGE(F2_D);
  CHECK_F_STORAGE(F3_D);
  CHECK_F_STORAGE(F4_D);
  CHECK_F_STORAGE(F5_D);
  CHECK_F_STORAGE(F6_D);
  CHECK_F_STORAGE(F7_D);
  CHECK_F_STORAGE(F8_D);
  CHECK_F_STORAGE(F9_D);
  CHECK_F_STORAGE(F10_D);
  CHECK_F_STORAGE(F11_D);
  CHECK_F_STORAGE(F12_D);
  CHECK_F_STORAGE(F13_D);
  CHECK_F_STORAGE(F14_D);
  CHECK_F_STORAGE(F15_D);
  CHECK_F_STORAGE(F16_D);
  CHECK_F_STORAGE(F17_D);
  CHECK_F_STORAGE(F18_D);
  CHECK_F_STORAGE(F19_D);
  CHECK_F_STORAGE(F20_D);
  CHECK_F_STORAGE(F21_D);
  CHECK_F_STORAGE(F22_D);
  CHECK_F_STORAGE(F23_D);
  CHECK_F_STORAGE(F24_D);
  CHECK_F_STORAGE(F25_D);
  CHECK_F_STORAGE(F26_D);
  CHECK_F_STORAGE(F27_D);
  CHECK_F_STORAGE(F28_D);
  CHECK_F_STORAGE(F29_D);
  CHECK_F_STORAGE(F30_D);
  CHECK_F_STORAGE(F31_D);
  CHECK_V_STORAGE(V0);
  CHECK_V_STORAGE(V1);
  CHECK_V_STORAGE(V2);
  CHECK_V_STORAGE(V3);
  CHECK_V_STORAGE(V4);
  CHECK_V_STORAGE(V5);
  CHECK_V_STORAGE(V6);
  CHECK_V_STORAGE(V7);
  CHECK_V_STORAGE(V8);
  CHECK_V_STORAGE(V9);
  CHECK_V_STORAGE(V10);
  CHECK_V_STORAGE(V11);
  CHECK_V_STORAGE(V12);
  CHECK_V_STORAGE(V13);
  CHECK_V_STORAGE(V14);
  CHECK_V_STORAGE(V15);
  CHECK_V_STORAGE(V16);
  CHECK_V_STORAGE(V17);
  CHECK_V_STORAGE(V18);
  CHECK_V_STORAGE(V19);
  CHECK_V_STORAGE(V20);
  CHECK_V_STORAGE(V21);
  CHECK_V_STORAGE(V22);
  CHECK_V_STORAGE(V23);
  CHECK_V_STORAGE(V24);
  CHECK_V_STORAGE(V25);
  CHECK_V_STORAGE(V26);
  CHECK_V_STORAGE(V27);
  CHECK_V_STORAGE(V28);
  CHECK_V_STORAGE(V29);
  CHECK_V_STORAGE(V30);
  CHECK_V_STORAGE(V31);
}

// Opcodes support
struct RISCVOpcodeSupported : public testing::SnippyState {};

namespace {
template <unsigned... Features> bool hasFeatures(const MCSubtargetInfo &SI) {
  return (SI.hasFeature(Features) && ...);
}
} // namespace

#define CHECK_OPCODE_SUPPORTED(Opc, ...)                                       \
  EXPECT_EQ(SnippyTarget.checkOpcodeSupported(llvm::RISCV::Opc, SI),           \
            (hasFeatures<__VA_ARGS__>(SI)))

#define CHECK_OPCODE_ALWAYS_SUPPORTED(Opc)                                     \
  EXPECT_TRUE(SnippyTarget.checkOpcodeSupported(llvm::RISCV::Opc, SI))

TEST_P(RISCVOpcodeSupported, RISCVOpcode) {
  const auto &SnippyTarget = State.getSnippyTarget();
  const auto &SI = *State.getMCContext().getSubtargetInfo();

  // Basic integer instructions (always supported)
  CHECK_OPCODE_ALWAYS_SUPPORTED(ADDI);
  CHECK_OPCODE_ALWAYS_SUPPORTED(ADD);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SUB);
  CHECK_OPCODE_ALWAYS_SUPPORTED(AND);
  CHECK_OPCODE_ALWAYS_SUPPORTED(OR);
  CHECK_OPCODE_ALWAYS_SUPPORTED(XOR);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SLL);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SRL);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SRA);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SLT);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SLTU);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LUI);
  CHECK_OPCODE_ALWAYS_SUPPORTED(AUIPC);
  CHECK_OPCODE_ALWAYS_SUPPORTED(JAL);
  CHECK_OPCODE_ALWAYS_SUPPORTED(JALR);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BEQ);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BNE);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BLT);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BGE);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BLTU);
  CHECK_OPCODE_ALWAYS_SUPPORTED(BGEU);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LB);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LH);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LW);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LBU);
  CHECK_OPCODE_ALWAYS_SUPPORTED(LHU);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SB);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SH);
  CHECK_OPCODE_ALWAYS_SUPPORTED(SW);
  CHECK_OPCODE_ALWAYS_SUPPORTED(FENCE);
  CHECK_OPCODE_ALWAYS_SUPPORTED(ECALL);
  CHECK_OPCODE_ALWAYS_SUPPORTED(EBREAK);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRW);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRS);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRC);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRWI);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRSI);
  CHECK_OPCODE_ALWAYS_SUPPORTED(CSRRCI);

  // Vector instructions
  CHECK_OPCODE_SUPPORTED(VSETVL, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSETVLI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSETIVLI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VLE8_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VLE16_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VLE32_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VLE64_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSE8_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSE16_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSE32_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSE64_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VADD_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VADD_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VADD_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSUB_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSUB_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VAND_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VAND_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VAND_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VOR_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VOR_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VOR_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VXOR_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VXOR_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VXOR_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSLL_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSLL_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSLL_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRL_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRL_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRL_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRA_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRA_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VSRA_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSEQ_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSEQ_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSEQ_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSNE_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSNE_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSNE_VI, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLTU_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLTU_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLT_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLT_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLEU_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLEU_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLE_VV, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMSLE_VX, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMERGE_VVM, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMERGE_VXM, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMERGE_VIM, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMV_V_V, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMV_V_X, FeatureStdExtV);
  CHECK_OPCODE_SUPPORTED(VMV_V_I, FeatureStdExtV);

  // Float instructions
  CHECK_OPCODE_SUPPORTED(FLW, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSW, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FADD_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSUB_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMUL_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FDIV_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSQRT_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMIN_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMAX_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMADD_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMSUB_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FNMADD_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FNMSUB_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSGNJ_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSGNJN_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FSGNJX_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FCVT_W_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FCVT_WU_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FCVT_S_W, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FCVT_S_WU, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FEQ_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FLT_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FLE_S, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMV_X_W, FeatureStdExtF);
  CHECK_OPCODE_SUPPORTED(FMV_W_X, FeatureStdExtF);

  // Double instructions
  CHECK_OPCODE_SUPPORTED(FLD, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSD, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FADD_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSUB_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FMUL_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FDIV_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSQRT_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FMIN_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FMAX_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FMADD_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FMSUB_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FNMADD_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FNMSUB_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSGNJ_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSGNJN_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FSGNJX_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_W_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_WU_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_D_W, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_D_WU, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_S_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FCVT_D_S, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FEQ_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FLT_D, FeatureStdExtD);
  CHECK_OPCODE_SUPPORTED(FLE_D, FeatureStdExtD);

  // Compressed instructions
  CHECK_OPCODE_SUPPORTED(C_ADDI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_ADDIW, FeatureStdExtC, Feature64Bit);
  CHECK_OPCODE_SUPPORTED(C_ADD, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_SUB, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_AND, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_OR, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_XOR, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_LW, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_SW, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_J, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_JAL, FeatureStdExtC, Feature32Bit);
  CHECK_OPCODE_SUPPORTED(C_JR, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_JALR, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_BEQZ, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_BNEZ, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_LI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_LUI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_SLLI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_SRLI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_SRAI, FeatureStdExtC);
  CHECK_OPCODE_SUPPORTED(C_ANDI, FeatureStdExtC);

  // Multiply instructions
  CHECK_OPCODE_SUPPORTED(MUL, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(MULH, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(MULHSU, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(MULHU, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(DIV, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(DIVU, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(REM, FeatureStdExtM);
  CHECK_OPCODE_SUPPORTED(REMU, FeatureStdExtM);

  // Atomic instructions
  CHECK_OPCODE_SUPPORTED(LR_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(SC_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOSWAP_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOADD_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOXOR_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOAND_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOOR_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOMIN_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOMAX_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOMINU_W, FeatureStdExtA);
  CHECK_OPCODE_SUPPORTED(AMOMAXU_W, FeatureStdExtA);
}

INSTANTIATE_TEST_SUITE_P(
    RISCVTarget, RISCVOpcodeSupported,
    ::testing::Values(SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/""},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d,+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+v,+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"",
                                         /*Features=*/"+f,+d,+v,+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/""},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d,+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+v,+c"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"",
                                         /*Features=*/"+f,+d,+v,+c"}));
} // namespace llvm::snippy
