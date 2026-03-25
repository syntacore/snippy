//===-- RegisterGenerator.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/RegisterGenerator.h"
#include "snippy/Generator/GeneratorContext.h"

#include <string>

namespace llvm {
namespace snippy {

static Register getRegFromIdx(const MCRegisterClass &RC,
                              ArrayRef<Register> Include, unsigned RegIdx) {
  if (RegIdx < RC.getNumRegs())
    return RC.getRegister(RegIdx);
  assert(RegIdx - RC.getNumRegs() < Include.size());
  return Include[RegIdx - RC.getNumRegs()];
}

static bool
regUnitIsReserved(unsigned RegUnitIdx, const SnippyTarget &SnippyTgt,
                  ArrayRef<Register> Exclude, const MCRegisterInfo &RI,
                  const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
                  AccessMaskBit Mask, const MCRegisterClass &RC,
                  ArrayRef<Register> Include) {
  auto RegUnit = getRegFromIdx(RC, Include, RegUnitIdx);
  SmallVector<Register> SubregsInc;
  SnippyTgt.getSubregsInclusive(RegUnit, RI, SubregsInc);
  if (any_of(SubregsInc, [&RP, &MBB, Mask](unsigned Reg) {
        return RP.isReserved(Reg, MBB, Mask);
      }))
    return true;
  return any_of(SubregsInc, [&Exclude](unsigned Reg) {
    return is_contained(Exclude, Reg);
  });
}

Expected<Register> RegisterGenerator::generateRandom(
    const SnippyTarget &SnippyTgt, const MCRegisterClass &RC,
    const MCRegisterInfo &RI, const RegPoolWrapper &RP,
    const MachineBasicBlock &MBB, ArrayRef<Register> Exclude,
    ArrayRef<Register> Include, AccessMaskBit Mask) const {
  // RegIdx may be greater than number of regs in REgClass because
  //  indexing includes Include registers
  auto MaxRegIdxValue = RC.getNumRegs() + Include.size() - 1;
  auto ExpectedRegIdx = RandEngine::genNUniqInInterval<unsigned>(
      0u, MaxRegIdxValue, /* N */ 1u,
      [&SnippyTgt, &Exclude, &RI, &RP, &MBB, Mask, &RC,
       Include](unsigned RegIdx) {
        return regUnitIsReserved(RegIdx, SnippyTgt, Exclude, RI, RP, MBB, Mask,
                                 RC, Include);
      });
  if (auto Err = ExpectedRegIdx.takeError()) {
    consumeError(std::move(Err));
    return make_error<NoAvailableRegister>(RC, RI, "instruction generation");
  }

  assert(ExpectedRegIdx->size() == 1);
  return getRegFromIdx(RC, Include, ExpectedRegIdx->front());
}

} // namespace snippy
} // namespace llvm
