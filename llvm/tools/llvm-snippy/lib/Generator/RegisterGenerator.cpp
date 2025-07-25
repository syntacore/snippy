//===-- RegisterGenerator.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "snippy/Generator/RegisterGenerator.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Support/DynLibLoader.h"
#include "llvm/ADT/ScopeExit.h"

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
  SmallVector<Register> RegsInUnit;
  SnippyTgt.getPhysRegsFromUnit(RegUnit, RI, RegsInUnit);
  if (any_of(RegsInUnit, [&RP, &MBB, Mask](unsigned Reg) {
        return RP.isReserved(Reg, MBB, Mask);
      }))
    return true;
  return any_of(RegsInUnit, [&Exclude](unsigned Reg) {
    return is_contained(Exclude, Reg);
  });
}

static bool regIsReserved(unsigned RegIdx, ArrayRef<Register> Exclude,
                          const RegPoolWrapper &RP,
                          const MachineBasicBlock &MBB, AccessMaskBit Mask,
                          const MCRegisterClass &RC,
                          ArrayRef<Register> Include) {
  Register Reg = getRegFromIdx(RC, Include, RegIdx);
  SmallVector<Register> PhysRegs;
  RP.getPhysRegsFromUnit(Reg, PhysRegs);
  return RP.isReserved(Reg, MBB, Mask) ||
         any_of(Exclude, [&PhysRegs](unsigned ExcludeReg) {
           return is_contained(PhysRegs, ExcludeReg);
         });
}

unsigned RegTranslatorHandle::getRegFromStr(
    const char *RegStr, const ::RegTranslatorHandle *RegTranslatorHandleObj) {
  auto *RegTranslator =
      reinterpret_cast<const RegTranslatorHandle *>(RegTranslatorHandleObj);
  assert(RegTranslator);
  RegTranslator->checkState();
  for (unsigned RegIdx = 0; RegIdx < RegTranslator->RC->getNumRegs();
       RegIdx++) {
    auto CurReg = RegTranslator->RC->getRegister(RegIdx);
    if (StringRef{RegStr} == StringRef{RegTranslator->RI->getName(CurReg)})
      return RegIdx;
  }
  constexpr unsigned InvalidReg = std::numeric_limits<unsigned>::max();
  return InvalidReg;
}

void RegTranslatorHandle::setState(const MCRegisterClass &RCIn,
                                   const MCRegisterInfo &RIIn) {
  RC = &RCIn;
  RI = &RIIn;
}

void RegTranslatorHandle::checkState() const {
  if (!RC || !RI)
    snippy::fatal("Bad RegTranslator state: one of the fields is nullptr");
}

void RegTranslatorHandle::clear() {
  RC = nullptr;
  RI = nullptr;
}

RegValidation RegVerifierHandle::regIsValidExtern(
    unsigned RegIdx, const ::RegVerifierHandle *RegVerifierHandleObj) {
  auto *RegVerifier =
      reinterpret_cast<const RegVerifierHandle *>(RegVerifierHandleObj);
  assert(RegVerifier);
  RegVerifier->checkState();

  auto MaxRegIdxValue = RegVerifier->getMaxRegIndex();
  if (RegIdx >= MaxRegIdxValue)
    return REG_IS_INVALID;

  if (regIsReserved(RegIdx, RegVerifier->Exclude, *RegVerifier->RP,
                    *RegVerifier->MBB, RegVerifier->Mask, *RegVerifier->RC,
                    RegVerifier->Include))
    return REG_IS_INVALID;
  return REG_IS_VALID;
}

void RegVerifierHandle::setState(const MCRegisterClass &RCIn,
                                 const MCRegisterInfo &RIIn,
                                 const RegPoolWrapper &RPIn,
                                 const MachineBasicBlock &MBBIn,
                                 ArrayRef<Register> ExcludeIn,
                                 ArrayRef<Register> IncludeIn,
                                 AccessMaskBit MaskIn) {
  assert(!RC && !RI && !MBB && !RP);
  RC = &RCIn;
  RI = &RIIn;
  MBB = &MBBIn;
  RP = &RPIn;
  Exclude = ExcludeIn;
  Include = IncludeIn;
  Mask = MaskIn;
}

bool RegVerifierHandle::regIsValid(unsigned RegIdx) const {
  auto *VerifierCastedPtr = reinterpret_cast<const ::RegVerifierHandle *>(this);
  return regIsValidExtern(RegIdx, VerifierCastedPtr) == REG_IS_VALID;
}

void RegVerifierHandle::checkState() const {
  if (!RC || !RI || !MBB || !RP)
    snippy::fatal("Bad RegVerifier state", "one of the fields is nullptr");
}

unsigned RegVerifierHandle::getMaxRegIndex() const {
  // RegIdx may be greater than number of regs in RegClass because
  //  indexing includes Include registers
  return RC->getNumRegs() + Include.size() - 1;
}

void RegVerifierHandle::clear() {
  RC = nullptr;
  RI = nullptr;
  MBB = nullptr;
  RP = nullptr;
  Exclude.clear();
  Include.clear();
  Mask = AccessMaskBit::None;
}

void RegisterGenerator::loadPluginDL(const std::string &PluginLibName) {
  auto Lib = DynamicLibrary(PluginLibName);
  const auto *VTable = reinterpret_cast<const RegPluginFunctionsTable *>(
      Lib.getAddressOfSymbol(REG_PLUGIN_ENTRY_NAME));
  if (!VTable)
    snippy::fatal("Can't find entry point of register plugin");
  DLTable = VTable;
}

void RegisterGenerator::setRegPluginInfo(
    const std::string &RegPluginInfoFile) const {
  assert(regGenIsWithPlugin() && "Plugin hasn't been loaded yet");

  if (DLTable->setRegInfoFile == nullptr)
    snippy::fatal("Invalid register plugin functions table",
                  "missing setRegInfoFile()");
  DLTable->setRegInfoFile(RegPluginInfoFile.c_str());
}

std::optional<Register> RegisterGenerator::generateWithPlugin(
    const MCRegisterClass &RC, const MCRegisterInfo &RI,
    const RegPoolWrapper &RP, const MachineBasicBlock &MBB,
    ArrayRef<Register> Exclude, ArrayRef<Register> Include,
    AccessMaskBit Mask) {
  RegVerifier.setState(RC, RI, RP, MBB, Exclude, Include, Mask);
  RegTranslator.setState(RC, RI);
  auto ClearRegVerifier = make_scope_exit([&] { RegVerifier.clear(); });
  auto ClearRegTranslator = make_scope_exit([&] { RegTranslator.clear(); });

  assert(DLTable);
  if (DLTable->generate == nullptr)
    snippy::fatal("Invalid register plugin functions table"
                  "missing generate()");
  auto PluginResp = DLTable->generate(RegVerifier.getMaxRegIndex());

  if (PluginResp.GenResult != REQUEST_IS_INVALID &&
      PluginResp.GenResult != REQUEST_IS_VALID)
    snippy::fatal("Incorrect reg plugin response");

  if (PluginResp.GenResult == REQUEST_IS_INVALID) {
    RequiresRollBack = true;
    return std::nullopt;
  }
  auto RegIdx = PluginResp.RegisterIdx;
  if (!RegVerifier.regIsValid(RegIdx))
    snippy::fatal("Invalid register index has been generated by reg plugin, "
                  "but GenResult was `REQUEST_IS_VALID`");

  return getRegFromIdx(RC, Include, RegIdx);
}

void RegisterGenerator::setRegContextForPlugin() {
  // in the new context, the old failures no longer matter
  RequiresRollBack = false;

  if (!regGenIsWithPlugin())
    return;
  RegVerifier.clear();
  RegTranslator.clear();
  auto *RegVerifierForContext =
      reinterpret_cast<::RegVerifierHandle *>(&RegVerifier);
  auto *RegTranslatorForContext =
      reinterpret_cast<::RegTranslatorHandle *>(&RegTranslator);
  auto RegPluginContext = SnippyRegContext{
      RegVerifierForContext, RegVerifierHandle::regIsValidExtern,
      RegTranslatorForContext, RegTranslatorHandle::getRegFromStr};
  if (DLTable->setContext == nullptr)
    snippy::fatal("Invalid register plugin functions table",
                  "missing setContext()");
  DLTable->setContext(RegPluginContext);
}

std::optional<Register> RegisterGenerator::generateRandom(
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
  if (!ExpectedRegIdx)
    snippy::fatal(
        make_error<NoAvailableRegister>(RC, RI, "instruction generation"));

  assert(ExpectedRegIdx->size() == 1);
  return getRegFromIdx(RC, Include, ExpectedRegIdx->front());
}

} // namespace snippy
} // namespace llvm
