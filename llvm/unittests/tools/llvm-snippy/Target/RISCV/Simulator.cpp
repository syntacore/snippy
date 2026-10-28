//===-- Simulator.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for Simulator/Targets/RISCV.h.
///
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/Targets/RISCV.h"

#include "snippy/unittests/SnippyState.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <utility>

namespace llvm::snippy {

static void PrintTo(const RISCVRegisterState &RS, std::ostream *OS) {
  std::string Str;
  raw_string_ostream SS(Str);
  RS.saveAsYAMLFile(SS);
  *OS << Str;
}

struct RISCVRegToIndex : public testing::SnippyState {
  RISCVRegToIndex() : SnippyState("riscv64", "", "", "") {}
};

TEST_F(RISCVRegToIndex, TestMatchesEncoding) {
  const auto &RI = State.getRegInfo();
  for (const auto *RC :
       {&RISCV::GPRRegClass, &RISCV::FPR16RegClass, &RISCV::FPR32RegClass,
        &RISCV::FPR64RegClass, &RISCV::VRRegClass}) {
    EXPECT_EQ(RC->getNumRegs(), RISCVRegisterState::NRegs);
    for (MCPhysReg Reg : *RC)
      EXPECT_EQ(regToIndex(Reg), RI.getEncodingValue(Reg)) << RI.getName(Reg);
  }
}

MATCHER_P(IsSysRegNamed, Name, "") {
  const auto *SysReg = RISCVSimulatorSysRegs::lookupSysReg(arg);
  if (!SysReg) {
    *result_listener << "which is not a known system register";
    return false;
  }
  *result_listener << "which is " << SysReg->Name;
  return StringRef(SysReg->Name) == Name;
}

TEST(RISCVSimulatorSysRegs, TestLookupSysReg) {
  EXPECT_THAT(RISCVSimulatorSysReg::FFLAGS, IsSysRegNamed("fflags"));
  EXPECT_THAT(RISCVSimulatorSysReg::FRM, IsSysRegNamed("frm"));
  EXPECT_THAT(RISCVSimulatorSysReg::FCSR, IsSysRegNamed("fcsr"));
  EXPECT_THAT(RISCVSimulatorSysReg::VSTART, IsSysRegNamed("vstart"));
  EXPECT_THAT(RISCVSimulatorSysReg::VXSAT, IsSysRegNamed("vxsat"));
  EXPECT_THAT(RISCVSimulatorSysReg::VXRM, IsSysRegNamed("vxrm"));
  EXPECT_THAT(RISCVSimulatorSysReg::VCSR, IsSysRegNamed("vcsr"));
  EXPECT_THAT(RISCVSimulatorSysReg::JVT, IsSysRegNamed("jvt"));
  EXPECT_THAT(RISCVSimulatorSysReg::MSTATUS, IsSysRegNamed("mstatus"));
}

TEST(RISCVRegisterState, TestMaxRegValueForSize) {
  EXPECT_EQ(RISCVRegisterState::getMaxRegValueForSize(Reg2Bytes), 0xFFFFu);
  EXPECT_EQ(RISCVRegisterState::getMaxRegValueForSize(Reg4Bytes), 0xFFFF'FFFFu);
  EXPECT_EQ(RISCVRegisterState::getMaxRegValueForSize(Reg8Bytes),
            0xFFFF'FFFF'FFFF'FFFFu);
}

struct RISCVSimulatorHelpers : public testing::SnippyState {
  Module M{"simulator_helpers", State.getCtx()};
  const RISCVSubtarget &ST = State.getSubtarget<RISCVSubtarget>(
      State.createFunction(M, "dummy", "", Function::ExternalLinkage));
};

TEST_P(RISCVSimulatorHelpers, TestRegSize) {
  const auto &RI = State.getRegInfo();
  auto XRegSize = ST.is64Bit() ? Reg8Bytes : Reg4Bytes;
  for (auto [RC, Size] : {std::pair{&RISCV::GPRRegClass, XRegSize},
                          std::pair{&RISCV::FPR16RegClass, Reg2Bytes},
                          std::pair{&RISCV::FPR32RegClass, Reg4Bytes},
                          std::pair{&RISCV::FPR64RegClass, Reg8Bytes}})
    for (MCPhysReg Reg : *RC)
      EXPECT_EQ(RISCVRegisterState::getRegSizeInBytes(ST, Reg, /*VLen=*/0),
                Size)
          << RI.getName(Reg);

  for (MCPhysReg Reg : RISCV::VRRegClass)
    EXPECT_EQ(getRegBitWidth(ST, Reg, /*VLEN=*/1024), 1024u) << RI.getName(Reg);
}

TEST_P(RISCVSimulatorHelpers, TestSysRegBitWidth) {
  for (auto [Reg, Width] :
       {std::pair{RISCVSimulatorSysReg::FFLAGS, 5u},
        std::pair{RISCVSimulatorSysReg::FRM, 3u},
        std::pair{RISCVSimulatorSysReg::FCSR, 32u},
        std::pair{RISCVSimulatorSysReg::VSTART, ST.getXLen()},
        std::pair{RISCVSimulatorSysReg::VXSAT, 1u},
        std::pair{RISCVSimulatorSysReg::VXRM, 2u},
        std::pair{RISCVSimulatorSysReg::VCSR, 3u},
        std::pair{RISCVSimulatorSysReg::JVT, ST.getXLen()},
        std::pair{RISCVSimulatorSysReg::MSTATUS, ST.getXLen()}})
    EXPECT_EQ(RISCVSimulatorSysRegs::getBitWidth(ST, Reg), Width)
        << RISCVSimulatorSysRegs::lookupSysReg(Reg)->Name;
}

TEST_P(RISCVSimulatorHelpers, TestSupportedSysRegs) {
  SmallVector<RISCVSimulatorSysReg> Expected{RISCVSimulatorSysReg::MSTATUS};
  if (ST.hasStdExtF())
    Expected.append({RISCVSimulatorSysReg::FFLAGS, RISCVSimulatorSysReg::FRM,
                     RISCVSimulatorSysReg::FCSR});
  if (ST.hasStdExtV())
    Expected.append({RISCVSimulatorSysReg::VSTART, RISCVSimulatorSysReg::VXSAT,
                     RISCVSimulatorSysReg::VXRM, RISCVSimulatorSysReg::VCSR});
  if (ST.hasStdExtZcmt())
    Expected.push_back(RISCVSimulatorSysReg::JVT);
  EXPECT_THAT(getSupportedSysRegs(ST),
              ::testing::UnorderedElementsAreArray(Expected));
}

TEST_P(RISCVSimulatorHelpers, TestRegisterStateConstruction) {
  constexpr unsigned VLEN = 512;
  RISCVRegisterState RS(ST, VLEN);
  EXPECT_EQ(RS.XRegSize, ST.is64Bit() ? Reg8Bytes : Reg4Bytes);
  EXPECT_EQ(RS.FRegSize, ST.hasStdExtD() ? Reg8Bytes : Reg4Bytes);

  EXPECT_EQ(RS.XRegs.size(), RISCVRegisterState::NRegs);
  EXPECT_EQ(RS.FRegs.size(), ST.hasStdExtF() ? RISCVRegisterState::NRegs : 0u);
  EXPECT_EQ(RS.VRegs.size(), ST.hasStdExtV() ? RISCVRegisterState::NRegs : 0u);
  for (const auto &VReg : RS.VRegs)
    EXPECT_EQ(VReg.getBitWidth(), VLEN);
}

TEST_P(RISCVSimulatorHelpers, TestRegisterStateRandomize) {
  RISCVRegisterState Initial(ST);
  auto RS = Initial;
  RS.randomize();
  EXPECT_NE(RS, Initial);

  EXPECT_EQ(RS.XRegs[0], 0u);
  EXPECT_THAT(RS.XRegs,
              ::testing::Each(::testing::Le(
                  RISCVRegisterState::getMaxRegValueForSize(RS.XRegSize))));
  EXPECT_THAT(RS.FRegs,
              ::testing::Each(::testing::Le(
                  RISCVRegisterState::getMaxRegValueForSize(RS.FRegSize))));

}

TEST_P(RISCVSimulatorHelpers, TestRegisterStateEquality) {
  RISCVRegisterState RS(ST);
  RS.randomize();
  auto Copy = RS;
  EXPECT_EQ(RS, Copy);

  Copy.XRegs[1] ^= 1;
  EXPECT_NE(RS, Copy);

  if (ST.hasStdExtF()) {
    Copy = RS;
    Copy.FRegs[0] ^= 1;
    EXPECT_NE(RS, Copy);
  }
  if (ST.hasStdExtV()) {
    Copy = RS;
    Copy.VRegs[0].flipBit(0);
    EXPECT_NE(RS, Copy);
  }
}

INSTANTIATE_TEST_SUITE_P(
    RISCVSimulator, RISCVSimulatorHelpers,
    ::testing::Values(SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/""},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d,+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+zcmt"},
                      SelectedTargetInfo{/*Triple=*/"riscv32", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+zcmt"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/""},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+f,+d,+v"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"", /*Features=*/"+zcmt"},
                      SelectedTargetInfo{/*Triple=*/"riscv64", /*MArch=*/"",
                                         /*CPU=*/"",
                                         /*Features=*/"+f,+zcmt"}));
} // namespace llvm::snippy
