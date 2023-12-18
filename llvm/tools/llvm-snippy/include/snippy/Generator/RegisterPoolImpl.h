//===-- RegisterPoolImpl.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/RandUtil.h"

#include <utility>

namespace llvm {
namespace snippy {

class RegPool;
class LocalRegPool;
class RegPoolWrapper;

class AvailableRegisterImpl {
  friend LocalRegPool;
  friend RegPool;
  friend RegPoolWrapper;

  template <class Pool, typename... Args>
  static unsigned getOne(const Twine &Desc, const MCRegisterInfo &RI,
                         const Pool &P, const MCRegisterClass &RegClass,
                         Args &&...OtherArgs) {
    return get(Desc, RI, P, /* NumRegs */ 1, RegClass,
               std::forward<Args>(OtherArgs)...)
        .front();
  }

  template <class Pool, typename Pred, typename... Args>
  static unsigned getOne(const Twine &Desc, const MCRegisterInfo &RI,
                         const Pool &P, Pred Filter,
                         const MCRegisterClass &RegClass, Args &&...OtherArgs) {
    return get(Desc, RI, P, Filter, /* NumRegs */ 1, RegClass,
               std::forward<Args>(OtherArgs)...)
        .front();
  }

  template <class Pool, typename... Args>
  static auto get(const Twine &Desc, const MCRegisterInfo &RI, const Pool &P,
                  unsigned NumRegs, const MCRegisterClass &RegClass,
                  Args &&...OtherArgs) {
    return get(
        Desc, RI, P, [](unsigned Reg) { return false; }, NumRegs, RegClass,
        std::forward<Args>(OtherArgs)...);
  }

  template <class Pool, typename... Args>
  static auto getAll(const Pool &P, const MCRegisterClass &RegClass,
                     Args &&...OtherArgs) {
    return getAll(
        P, [](unsigned Reg) { return false; }, RegClass,
        std::forward<Args>(OtherArgs)...);
  }

  template <size_t N, class Pool, typename... Args>
  static auto get(const Twine &Desc, const MCRegisterInfo &RI, const Pool &P,
                  const MCRegisterClass &RegClass, Args &&...OtherArgs) {
    return get<N>(
        Desc, RI, P, [](unsigned Reg) { return false; }, RegClass,
        std::forward<Args>(OtherArgs)...);
  }

  template <class Pool, typename Pred, typename OutItType, typename... Args>
  static auto getAllImpl(const Pool &P, Pred Filter, OutItType OutputIt,
                         const MCRegisterClass &RegClass, Args &&...OtherArgs) {

    auto CombinedFilter = [&](unsigned Idx) {
      auto R = RegClass.getRegister(Idx);
      return P.isReserved(R, std::forward<Args>(OtherArgs)...) ||
             std::invoke(Filter, R);
    };

    auto NumRegs = RandEngine::countUniqInInterval(
        0u, RegClass.getNumRegs() - 1, CombinedFilter);
    assert(NumRegs && "countUniqInInterval must always succeed");

    auto RegIdxs = RandEngine::genNUniqInInterval(0u, RegClass.getNumRegs() - 1,
                                                  *NumRegs, CombinedFilter);
    assert(RegIdxs && "can't get all available unique values");

    transform(*RegIdxs, OutputIt,
              [&RegClass](auto Idx) { return RegClass.getRegister(Idx); });
  }

  template <class Pool, typename Pred, typename OutItType, typename... Args>
  static auto getImpl(const Twine &Desc, const MCRegisterInfo &RI,
                      const Pool &P, Pred Filter, unsigned NumRegs,
                      OutItType OutputIt, const MCRegisterClass &RegClass,
                      Args &&...OtherArgs) {

    auto RegIdxs = RandEngine::genNUniqInInterval(
        0u, RegClass.getNumRegs() - 1, NumRegs, [&](unsigned Idx) {
          auto R = RegClass.getRegister(Idx);
          return P.isReserved(R, std::forward<Args>(OtherArgs)...) ||
                 std::invoke(Filter, R);
        });

    if (!RegIdxs)
      report_fatal_error(make_error<NoAvailableRegister>(RegClass, RI, Desc));

    transform(*RegIdxs, OutputIt,
              [&RegClass](auto Idx) { return RegClass.getRegister(Idx); });
  }

  template <class Pool, typename Pred, typename... Args>
  static std::vector<unsigned>
  get(const Twine &Desc, const MCRegisterInfo &RI, const Pool &P, Pred Filter,
      unsigned NumRegs, const MCRegisterClass &RegClass, Args &&...OtherArgs) {
    std::vector<unsigned> Regs;
    // NOTE: we could reserve space and use Regs.begin(), but we don't :)
    getImpl(Desc, RI, P, std::move(Filter), NumRegs, std::back_inserter(Regs),
            RegClass, std::forward<Args>(OtherArgs)...);

    return Regs;
  }

  template <class Pool, typename Pred, typename... Args>
  static std::vector<unsigned> getAll(const Pool &P, Pred Filter,
                                      const MCRegisterClass &RegClass,
                                      Args &&...OtherArgs) {
    std::vector<unsigned> Regs;
    // NOTE: we could reserve space and use Regs.begin(), but we don't :)
    getAllImpl(P, std::move(Filter), std::back_inserter(Regs), RegClass,
               std::forward<Args>(OtherArgs)...);
    return Regs;
  }

  template <size_t N, class Pool, typename Pred, typename... Args>
  static std::array<unsigned, N>
  get(const Twine &Desc, const MCRegisterInfo &RI, const Pool &P, Pred Filter,
      const MCRegisterClass &RegClass, Args &&...OtherArgs) {
    std::array<unsigned, N> Regs;
    getImpl(Desc, RI, P, std::move(Filter), N, Regs.begin(), RegClass,
            std::forward<Args>(OtherArgs)...);
    return Regs;
  }
};

} // namespace snippy
} // namespace llvm
