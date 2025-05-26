//===-- FPRNaNIdentifier.h -------------------------------------- *- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SNIPPY_INCLUDE_CONFIG_FPR_NAN_IDENTIFIER_H
#define LLVM_SNIPPY_INCLUDE_CONFIG_FPR_NAN_IDENTIFIER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/MC/MCRegister.h"
#include "llvm/MC/MCRegisterInfo.h"

namespace llvm {
namespace snippy {

enum class FPRNaNState { NOT_NAN, POSSIBLY_NAN, DEFINITELY_NAN };

class NaNIdentifier final {

  using RegClassStorage = llvm::SmallSet<const MCRegisterClass *, 2>;
  // Store all possible RegClasses for MCRegister with his state
  using MapPair = std::pair<FPRNaNState, RegClassStorage>;

public:
  using size_type = unsigned;

  NaNIdentifier(size_type MapSize)
      : RegsNum(MapSize), FPRegsInfo(RegsNum), ClassToStates(3) {}

  void markRegisterWithNaNState(MCRegister Reg, const MCRegisterClass &RegClass,
                                FPRNaNState State);

  auto getRegsInState(const MCRegisterClass &RegClass,
                      FPRNaNState State = FPRNaNState::POSSIBLY_NAN) {
    return llvm::make_first_range(
        llvm::make_filter_range(FPRegsInfo, [&, State](auto &MapInfo) {
          auto &[CurrState, RegClasses] = MapInfo.second;
          return RegClasses.count(&RegClass) && State == CurrState;
        }));
  }

  // Returns register classes corresponding to the provided register Reg
  auto getMCRegClasses(MCRegister Reg) const {
    auto It = FPRegsInfo.find(Reg);
    return It != FPRegsInfo.end() ? It->second.second : RegClassStorage();
  }

  // Populates the output parameter ClassesToFill with the machine register
  // classes corresponding to the registers provided in the input Regs
  void getMCRegClasses(
      ArrayRef<MCRegister> Regs,
      SmallVectorImpl<const MCRegisterClass *> &ClassesToFill) const;

  // Returns the number of registers related to the passed RegClass that have
  // any of the passed NaNStates.
  size_type getNaNStateRegistersCount(const MCRegisterClass &RegClass,
                                      ArrayRef<FPRNaNState> NaNStates) const;

  bool contains(MCRegister Reg) const { return FPRegsInfo.contains(Reg); }

  bool containsWithState(MCRegister Reg, FPRNaNState RegState) const {
    auto It = FPRegsInfo.find(Reg);
    if (It == FPRegsInfo.end())
      return false;
    return It->second.first == RegState;
  }

  bool isNotNaN(MCRegister Reg) const {
    return containsWithState(Reg, FPRNaNState::NOT_NAN);
  }

  bool isPotentiallyNaN(MCRegister Reg) const {
    return containsWithState(Reg, FPRNaNState::POSSIBLY_NAN);
  }

  bool isDefinitelyNaN(MCRegister Reg) const {
    return containsWithState(Reg, FPRNaNState::DEFINITELY_NAN);
  }

  bool empty() const { return FPRegsInfo.empty(); }

  [[nodiscard]] size_type capacity() const { return RegsNum; }

private:
  size_type RegsNum;
  DenseMap<MCRegister, MapPair> FPRegsInfo;
  // This field stores the total count of FPRNaNState entries for each RegClass.
  DenseMap<const MCRegisterClass *, DenseMap<FPRNaNState, size_type>>
      ClassToStates;
};

} // namespace snippy
} // namespace llvm

#endif
