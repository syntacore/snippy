//===-- StepEffectsObserver.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/StepEffectsObserver.h"
#include "snippy/Simulator/Simulator.h"
#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <map>

namespace llvm {
namespace snippy {

void StepEffects::clear() {
  XRegs.clear();
  FRegs.clear();
  VRegs.clear();
  CSRs.clear();
  WrittenMem.clear();
  ReadMem.clear();
}

void StepEffectsObserver::memUpdateNotification(MemoryAddressType Addr,
                                                const char *Data, size_t Size) {
  for (auto Offset : seq<size_t>(0, Size))
    Effects.WrittenMem[Addr + Offset] = Data[Offset];
}

void StepEffectsObserver::memReadNotification(MemoryAddressType Addr,
                                              const char *Data, size_t Size) {
  for (auto Offset : seq<size_t>(0, Size))
    Effects.ReadMem.try_emplace(Addr + Offset, Data[Offset]);
}

void StepEffectsObserver::xregUpdateNotification(unsigned RegID,
                                                 RegisterType Value) {
  Effects.XRegs.insert(RegID);
}

void StepEffectsObserver::fregUpdateNotification(unsigned RegID,
                                                 RegisterType Value) {
  Effects.FRegs.insert(RegID);
}

void StepEffectsObserver::vregUpdateNotification(unsigned RegID,
                                                 ArrayRef<char> Data) {
  Effects.VRegs.insert(RegID);
}

void StepEffectsObserver::csrUpdateNotification(unsigned RegID,
                                                RegisterType Value) {
  if (is_contained(TrackedCSRs, RegID))
    Effects.CSRs.insert(RegID);
}

static bool isSameValue(const VectorRegisterType &Value,
                        const VectorRegisterType &AnotherValue) {
  return Value.getBitWidth() == AnotherValue.getBitWidth() &&
         Value == AnotherValue;
}

static bool isSameValue(RegisterType Value, RegisterType AnotherValue) {
  return Value == AnotherValue;
}

static std::string toHexString(RegisterType Value) {
  return "0x" + utohexstr(Value);
}

static std::string toHexString(const VectorRegisterType &Value) {
  auto Digits = toString(Value, /* Radix */ 16, /* Signed */ false);
  auto Width = divideCeil(Value.getBitWidth(), 4);
  return "0x" +
         std::string(Width - std::min<size_t>(Width, Digits.size()), '0') +
         Digits;
}

static RegisterType readXReg(const SimulatorInterface &Sim, unsigned RegID) {
  auto Value = Sim.readGPR(RegID);
  return SNIPPY_UNWRAP_EXPECTED(Value, "failed to read register");
}

static RegisterType readFReg(const SimulatorInterface &Sim, unsigned RegID) {
  auto Value = Sim.readFPR(RegID);
  return SNIPPY_UNWRAP_EXPECTED(Value, "failed to read register");
}

static VectorRegisterType readVReg(const SimulatorInterface &Sim,
                                   unsigned RegID) {
  auto Value = Sim.readVPR(RegID);
  return SNIPPY_UNWRAP_EXPECTED(Value, "failed to read register");
}

static RegisterType readCSR(const SimulatorInterface &Sim, unsigned RegID) {
  auto Value = Sim.readCSR(RegID);
  return SNIPPY_UNWRAP_EXPECTED(Value, "failed to read register");
}

static char readMemByte(const SimulatorInterface &Sim, MemoryAddressType Addr) {
  char Byte = 0;
  auto Err = Sim.readMem(Addr, Byte);
  SNIPPY_CHECK_ERROR(Err, "failed to read memory");
  return Byte;
}

using MemBytesMismatch = std::map<MemoryAddressType, std::pair<char, char>>;

// Collects the bytes either model reported as written that differ between
// the models. Bytes a model has not reported are read from it.
static MemBytesMismatch diffWrittenMem(const StepEffects &Effects,
                                       const SimulatorInterface &Sim,
                                       const StepEffects &AnotherEffects,
                                       const SimulatorInterface &AnotherSim) {
  MemBytesMismatch Different;
  for (auto [Addr, Byte] : Effects.WrittenMem) {
    auto It = AnotherEffects.WrittenMem.find(Addr);
    auto AnotherByte = It != AnotherEffects.WrittenMem.end()
                           ? It->second
                           : readMemByte(AnotherSim, Addr);
    if (Byte != AnotherByte)
      Different[Addr] = {Byte, AnotherByte};
  }

  for (auto [Addr, AnotherByte] : AnotherEffects.WrittenMem) {
    if (Effects.WrittenMem.contains(Addr))
      continue;
    auto Byte = readMemByte(Sim, Addr);
    if (Byte != AnotherByte)
      Different[Addr] = {Byte, AnotherByte};
  }
  return Different;
}

// Adds a mismatch for each run of consecutive addresses.
static void appendMemMismatches(StringRef Kind, const MemBytesMismatch &Bytes,
                                std::vector<EffectMismatch> &Mismatches) {
  auto ToHex = [](char Byte) {
    return utohexstr(static_cast<unsigned char>(Byte), /* LowerCase */ false,
                     /* Width */ 2);
  };

  for (auto It = Bytes.begin(); It != Bytes.end();) {
    auto Start = It->first;
    SmallVector<std::string> Values;
    SmallVector<std::string> AnotherValues;
    for (auto Addr = Start; It != Bytes.end() && It->first == Addr;
         ++It, ++Addr) {
      Values.push_back(ToHex(It->second.first));
      AnotherValues.push_back(ToHex(It->second.second));
    }
    Mismatches.push_back({formatv("{0} at 0x{1}", Kind, utohexstr(Start)),
                          join(Values, " "), join(AnotherValues, " ")});
  }
}

std::vector<EffectMismatch>
compareStepEffects(const StepEffects &Effects, const SimulatorInterface &Sim,
                   const StepEffects &AnotherEffects,
                   const SimulatorInterface &AnotherSim) {
  std::vector<EffectMismatch> Mismatches;

  auto PC = Sim.readPC();
  auto AnotherPC = AnotherSim.readPC();
  if (PC != AnotherPC)
    Mismatches.push_back({"pc", toHexString(PC), toHexString(AnotherPC)});

  auto CompareRegs = [&](StringRef Prefix, const DenseSet<unsigned> &Regs,
                         const DenseSet<unsigned> &AnotherRegs, auto ReadReg) {
    SmallVector<std::pair<unsigned, EffectMismatch>> Different;
    auto Compare = [&](unsigned RegID) {
      auto Value = ReadReg(Sim, RegID);
      auto AnotherValue = ReadReg(AnotherSim, RegID);
      if (!isSameValue(Value, AnotherValue))
        Different.push_back({RegID,
                             {(Prefix + Twine(RegID)).str(), toHexString(Value),
                              toHexString(AnotherValue)}});
    };
    for_each(Regs, Compare);
    for (auto RegID : AnotherRegs)
      if (!Regs.contains(RegID))
        Compare(RegID);
    sort(Different, less_first());
    append_range(Mismatches, make_second_range(Different));
  };
  CompareRegs("x", Effects.XRegs, AnotherEffects.XRegs, readXReg);
  CompareRegs("f", Effects.FRegs, AnotherEffects.FRegs, readFReg);
  CompareRegs("v", Effects.VRegs, AnotherEffects.VRegs, readVReg);
  CompareRegs("csr", Effects.CSRs, AnotherEffects.CSRs, readCSR);

  appendMemMismatches("memory write",
                      diffWrittenMem(Effects, Sim, AnotherEffects, AnotherSim),
                      Mismatches);

  MemBytesMismatch DifferentReads;
  for (auto [Addr, Byte] : Effects.ReadMem) {
    auto It = AnotherEffects.ReadMem.find(Addr);
    if (It != AnotherEffects.ReadMem.end() && It->second != Byte)
      DifferentReads[Addr] = {Byte, It->second};
  }
  appendMemMismatches("memory read", DifferentReads, Mismatches);

  return Mismatches;
}

} // namespace snippy
} // namespace llvm
