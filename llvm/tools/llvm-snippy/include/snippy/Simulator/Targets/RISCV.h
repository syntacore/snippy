//===-- RISCV.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TARGETS_RISCV_H
#define LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TARGETS_RISCV_H

#include "../Simulator.h"

#include "RISCVSubtarget.h"

#include "snippy/Support/DynLibLoader.h"

namespace llvm {
namespace snippy {

constexpr static auto RISCV_CHAR_BIT = 8u;
constexpr static auto kMaxSupportedInstrSize = 4u;
constexpr static auto kCompressedInstrSize = 2u;

enum RegSizeInBytes {
  Reg2Bytes = 2,
  Reg4Bytes = 4,
  Reg8Bytes = 8,
};

/// Numeric values correspond to the CSR encoding specified by
/// the RISC-V ISA spec.
namespace RISCVSimulatorSysRegs {
enum RISCVSimulatorSysReg : uint16_t {
  FFLAGS = 0x001,
  FRM = 0x002,
  FCSR = 0x003,
  // Table jump base vector and control register
  JVT = 0x017,
};

/// Lookup SysReg by its encoding.
const RISCVSysReg::SysReg *lookupSysReg(RISCVSimulatorSysReg Reg);

/// Get system register used bit width as defined in the ISA spec.
unsigned getBitWidth(const RISCVSubtarget &ST, RISCVSimulatorSysReg Reg);
} // namespace RISCVSimulatorSysRegs

/// \brief Get list of all supported system registers.
SmallVector<RISCVSimulatorSysRegs::RISCVSimulatorSysReg>
getSupportedSysRegs(const RISCVSubtarget &ST);

static inline unsigned getRegBitWidth(const RISCVSubtarget &ST, MCRegister Reg,
                                      unsigned VLEN = 0) {
  if (RISCV::GPRRegClass.contains(Reg))
    return ST.getXLen();
  if (RISCV::FPR16RegClass.contains(Reg))
    return Reg2Bytes * RISCV_CHAR_BIT;
  if (RISCV::FPR32RegClass.contains(Reg))
    return Reg4Bytes * RISCV_CHAR_BIT;
  if (RISCV::FPR64RegClass.contains(Reg))
    return Reg8Bytes * RISCV_CHAR_BIT;
  auto RegID = Reg.id();
  if (is_contained(getSupportedSysRegs(ST), RegID))
    return RISCVSimulatorSysRegs::getBitWidth(
        ST, static_cast<RISCVSimulatorSysRegs::RISCVSimulatorSysReg>(Reg.id()));
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

using RISCVSimulatorSysRegs::RISCVSimulatorSysReg;

struct RISCVRegisterState : public IRegisterState {
  const RISCVSubtarget *ST;
  unsigned VLEN;
  unsigned VLENB;
  static constexpr unsigned NRegs = 32;
  RegSizeInBytes XRegSize = Reg4Bytes;
  RegSizeInBytes FRegSize = Reg4Bytes;

  ProgramCounterType PC = 0;
  std::vector<uint64_t> XRegs;
  std::vector<uint64_t> FRegs;
  std::vector<APInt> VRegs;

  RISCVRegisterState(const RISCVSubtarget &ST,
                     unsigned VLEN = 16 * RISCV_CHAR_BIT)
      : ST(&ST), VLEN(VLEN), VLENB(VLEN / RISCV_CHAR_BIT), XRegs(NRegs) {
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

  void loadFromYamlFile(StringRef YamlFile, WarningsT &WarningsArr,
                        const SnippyTarget *Tgt = nullptr) override;

  void saveAsYAMLFile(raw_ostream &OS) const override;

  bool operator==(const IRegisterState &) const override;

  static uint64_t getMaxRegValueForSize(RegSizeInBytes Size);

  uint64_t getMaxRegValueForSize(Register Reg, unsigned VLen) const;

  static uint64_t getMaxRegValueForSize(const RISCVSubtarget &ST, Register Reg,
                                        unsigned VLen);

  RegSizeInBytes getRegSizeInBytes(Register Reg, unsigned VLen) const;

  static RegSizeInBytes getRegSizeInBytes(const RISCVSubtarget &ST,
                                          Register Reg, unsigned VLen);

private:
  void uniformlyFillXRegs();
  void uniformlyFillFRegs();
  void uniformlyFillVRegs();
};

std::unique_ptr<SimulatorInterface> createRISCVSimulator(
    llvm::snippy::DynamicLibrary &ModelLib, const SimulationConfig &Cfg,
    RVMCallbackHandler *CallbackHandler, const RISCVSubtarget &Subtarget,
    unsigned VLENB = 0, bool EnableMisalignedAccess = false);
} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_TARGETS_RISCV_H
