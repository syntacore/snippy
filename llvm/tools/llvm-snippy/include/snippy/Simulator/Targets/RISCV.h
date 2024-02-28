//===-- RISCV.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../Simulator.h"

#include "RISCVSubtarget.h"

#include "snippy/Support/DynLibLoader.h"

namespace llvm {
namespace snippy {

constexpr static auto RISCV_CHAR_BIT = 8u;

enum RegSizeInBytes {
  Reg2Bytes = 2,
  Reg4Bytes = 4,
  Reg8Bytes = 8,
  Reg16Bytes = 16
};

static inline unsigned getRegBitWidth(MCRegister Reg, unsigned XLen,
                                      unsigned VLEN = 0) {
  if (RISCV::GPRRegClass.contains(Reg))
    return XLen;
  if (RISCV::FPR16RegClass.contains(Reg))
    return Reg2Bytes * RISCV_CHAR_BIT;
  if (RISCV::FPR32RegClass.contains(Reg))
    return Reg4Bytes * RISCV_CHAR_BIT;
  if (RISCV::FPR64RegClass.contains(Reg))
    return Reg8Bytes * RISCV_CHAR_BIT;
  assert(RISCV::VRRegClass.contains(Reg) && "unknown register class");
  return VLEN;
}

static inline unsigned regToIndex(Register Reg) {
  if (RISCV::X0 <= Reg && Reg <= RISCV::X31)
    return Reg - RISCV::X0;
  if (RISCV::F0_D <= Reg && Reg <= RISCV::F31_D)
    return Reg - RISCV::F0_D;
  if (RISCV::F0_F <= Reg && Reg <= RISCV::F31_F)
    return Reg - RISCV::F0_F;
  if (RISCV::F0_H <= Reg && Reg <= RISCV::F31_H)
    return Reg - RISCV::F0_H;
  assert(RISCV::V0 <= Reg && Reg <= RISCV::V31 && "unknown register");
  return Reg - RISCV::V0;
}

struct RISCVRegisterState : public IRegisterState {
  // FIXME: VLEN should be dynamic and derived from target configuration
  unsigned VLEN = Reg16Bytes * RISCV_CHAR_BIT;
  unsigned VLENB = VLEN / RISCV_CHAR_BIT;
  static constexpr unsigned NRegs = 32;
  RegSizeInBytes XRegSize = Reg4Bytes;
  RegSizeInBytes FRegSize = Reg4Bytes;

  ProgramCounterType PC;
  std::vector<uint64_t> XRegs;
  std::vector<uint64_t> FRegs;
  std::vector<APInt> VRegs;

  RISCVRegisterState(const RISCVSubtarget &ST) : XRegs(NRegs) {
    if (ST.is64Bit())
      XRegSize = RegSizeInBytes::Reg8Bytes;

    if (ST.hasStdExtF()) {
      FRegs.resize(NRegs);
      if (ST.hasStdExtD())
        FRegSize = RegSizeInBytes::Reg8Bytes;
    }

    if (ST.hasStdExtV())
      VRegs.resize(NRegs, APInt(VLEN, 0));
  }

  void randomize() override {
    uniformlyFillXRegs();
    uniformlyFillFRegs();
    uniformlyFillVRegs();
  }

  void loadFromYamlFile(StringRef YamlFile, WarningsT &WarningsArr) override;

  void saveAsYAMLFile(raw_ostream &OS) const override;

  bool operator==(const IRegisterState &) const override;

private:
  void uniformlyFillXRegs();
  void uniformlyFillFRegs();
  void uniformlyFillVRegs();

  static uint64_t getMaxRegValueForSize(RegSizeInBytes Size) {
    if (Size == RegSizeInBytes::Reg4Bytes)
      return std::numeric_limits<uint32_t>::max();
    return std::numeric_limits<uint64_t>::max();
  }
};

std::unique_ptr<SimulatorInterface> createRISCVSimulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const RISCVSubtarget &Subtarget,
    unsigned VLENB = 0, bool EnableMisalignedAccess = false);
} // namespace snippy
} // namespace llvm
