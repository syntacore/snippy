//===-- RegisterGenerator.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// RegisterGenerator - wrapper for the register plugin.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/GeneratorUtils/RegisterPool.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/Register.h"

#include <memory>
#include <vector>

namespace llvm {
namespace snippy {

class GeneratorContext;

class RegisterGenerator final {
  bool RequiresRollBack = false;

  Expected<Register>
  generateRandom(const SnippyTarget &SnippyTgt, const MCRegisterClass &RC,
                 const MCRegisterInfo &RI, const RegPoolWrapper &RP,
                 const MachineBasicBlock &MBB, ArrayRef<Register> Exclude,
                 ArrayRef<Register> Include, AccessMaskBit Mask) const;

public:
  // Returns register from random generator.
  Expected<Register>
  generate(const MCRegisterClass &RC, unsigned OperandRegClassID,
           const MCRegisterInfo &RI, const RegPoolWrapper &RP,
           const MachineBasicBlock &MBB, const SnippyTarget &SnippyTgt,
           ArrayRef<Register> Exclude = {}, ArrayRef<Register> Include = {},
           AccessMaskBit Mask = AccessMaskBit::RW) {
    assert(!RequiresRollBack && "Can't generate without failure recovery");
    return generateRandom(SnippyTgt, RC, RI, RP, MBB, Exclude, Include, Mask);
  }
};

} // namespace snippy
} // namespace llvm
