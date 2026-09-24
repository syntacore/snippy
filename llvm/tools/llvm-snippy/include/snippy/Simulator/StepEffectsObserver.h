//===-- StepEffectsObserver.h ------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_STEPEFFECTSOBSERVER_H
#define LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_STEPEFFECTSOBSERVER_H

#include "Observer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <string>
#include <vector>

namespace llvm {
namespace snippy {

class SimulatorInterface;

struct StepEffects {
  DenseSet<unsigned> XRegs;
  DenseSet<unsigned> FRegs;
  DenseSet<unsigned> VRegs;
  DenseSet<unsigned> CSRs;

  DenseMap<MemoryAddressType, char> WrittenMem;
  DenseMap<MemoryAddressType, char> ReadMem;

  void clear();
};

class StepEffectsObserver final : public Observer {
  StepEffects Effects;
  // Some CSRs are not initialized the same way in all models yet,
  // so they may legitimately differ.
  SmallVector<unsigned> TrackedCSRs;

public:
  StepEffectsObserver(ArrayRef<unsigned> TrackedCSRs = {})
      : TrackedCSRs(TrackedCSRs) {}

  void clear() { Effects.clear(); }

  const StepEffects &getEffects() const { return Effects; }

  void memUpdateNotification(MemoryAddressType Addr, const char *Data,
                             size_t Size) override;
  void memReadNotification(MemoryAddressType Addr, const char *Data,
                           size_t Size) override;
  void xregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void fregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void vregUpdateNotification(unsigned RegID, ArrayRef<char> Data) override;
  void csrUpdateNotification(unsigned RegID, RegisterType Value) override;
};

struct EffectMismatch {
  std::string Location;
  std::string Value;
  std::string AnotherValue;
};

std::vector<EffectMismatch>
compareStepEffects(const StepEffects &Effects, const SimulatorInterface &Sim,
                   const StepEffects &AnotherEffects,
                   const SimulatorInterface &AnotherSim);

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_SIMULATOR_STEPEFFECTSOBSERVER_H
