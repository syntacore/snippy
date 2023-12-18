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

#include "snippy/Generator/RegisterPool.h"
#include "snippy/Plugins/RegisterPluginCInterface.h"

#include "llvm/CodeGen/Register.h"

#include <memory>
#include <vector>

namespace llvm {
namespace snippy {

class GeneratorContext;

// Definition of the ::RegVerifierHandle from the RegisterPluginCInterface.h
// Includes all the infromation needed to determine
//  whether the register index is valid for the current operand
struct RegVerifierHandle {
  const MCRegisterClass *RC = nullptr;
  const MCRegisterInfo *RI = nullptr;
  const MachineBasicBlock *MBB = nullptr;
  const RegPoolWrapper *RP = nullptr;
  std::vector<Register> Exclude;
  std::vector<Register> Include;
  AccessMaskBit Mask;

public:
  static RegValidation
  regIsValidExtern(unsigned RegIdx,
                   const ::RegVerifierHandle *RegVerifierHandleObj);

  void setState(const MCRegisterClass &RCIn, const MCRegisterInfo &RIIn,
                const RegPoolWrapper &RPIn, const MachineBasicBlock &MBBIn,
                ArrayRef<Register> ExcludeIn, ArrayRef<Register> IncludeIn,
                AccessMaskBit MaskIn);

  bool regIsValid(unsigned RegIdx) const;

  void checkState() const;

  unsigned getMaxRegIndex() const;

  void clear();
};

// Definition of the ::RegTranslatorHandle from the RegisterPluginCInterface.h
// Includes all the information needed to translate string to register index
//  for the current operand
class RegTranslatorHandle {
  const MCRegisterClass *RC = nullptr;
  const MCRegisterInfo *RI = nullptr;

public:
  static unsigned
  getRegFromStr(const char *RegStr,
                const ::RegTranslatorHandle *RegTranslatorHandleObj);

  void setState(const MCRegisterClass &RCIn, const MCRegisterInfo &RIIn);

  void checkState() const;

  void clear();
};

class RegisterGenerator final {
  const RegPluginFunctionsTable *DLTable = nullptr;
  RegVerifierHandle RegVerifier;
  RegTranslatorHandle RegTranslator;
  bool RequiresRollBack = false;

  void loadPluginDL(const std::string &PluginLibName);

  void setRegPluginInfo(const std::string &RegPluginInfoFile) const;

  // Calls interface function generate()
  //  with the corresponding  SnippyRegContext.
  // After this call RegVerifier and RegTranslator are zero-initialized.
  std::optional<Register>
  generateWithPlugin(const MCRegisterClass &RC, const MCRegisterInfo &RI,
                     const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
                     ArrayRef<Register> Exclude, ArrayRef<Register> Include,
                     AccessMaskBit Mask);

  std::optional<Register>
  generateRandom(const MCRegisterClass &RC, const MCRegisterInfo &RI,
                 const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
                 ArrayRef<Register> Exclude, ArrayRef<Register> Include,
                 AccessMaskBit Mask) const;

public:
  RegisterGenerator(const std::string &PluginLibName,
                    const std::string &RegPluginInfoFile) {
    if (!PluginLibName.empty())
      loadPluginDL(PluginLibName);
    if (!RegPluginInfoFile.empty())
      setRegPluginInfo(RegPluginInfoFile);
  }

  bool regGenIsWithPlugin() const { return DLTable != nullptr; }

  // This function passes SnippyRegContext to the plugin if it is enabled
  void setRegContextForPlugin();

  // Returns register either from (future)plugin or from random generator.
  std::optional<Register>
  generate(const MCRegisterClass &RC, const MCRegisterInfo &RI,
           const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
           ArrayRef<Register> Exclude = {}, ArrayRef<Register> Include = {},
           AccessMaskBit Mask = AccessMaskBit::RW) {
    assert(!RequiresRollBack && "Can't generate without failure recovery");
    if (regGenIsWithPlugin())
      return generateWithPlugin(RC, RI, RP, MBB, Exclude, Include, Mask);
    return generateRandom(RC, RI, RP, MBB, Exclude, Include, Mask);
  }
};

} // namespace snippy
} // namespace llvm