//===-- RegisterPool.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This class implements a pool of registers from which generator can pick. The
/// main idea is next:
///   * LocalRegPool is a pool that is associated with some entity (whole
///     application, function, basic block).
///   * RegPool is a mix of LocalRegPools that fully describes reserved
///     registers.
///   * RegPoolWrapper is a wrapper around RegPool. Its purpose is to make easy
///     interface for creating RegPool wrappers. It works like stack: on copy a
///     new instance is pushed on top, and at the end of scope its top is
///     popped.
///   * GeneratorContext and all external functions and passes must work with
///     RegPoolWrappers.
///   * RegPool from the root RegPoolWrapper can be obtained from the
///     RootRegPoolWrapper pass.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "snippy/Generator/RegisterPoolImpl.h"
#include "snippy/Support/Error.h"
#include "snippy/Support/Utils.h"

#include <array>
#include <map>
#include <utility>

namespace llvm {

class MachineBasicBlock;
class MachineFunction;
class MachineLoop;
class MCRegisterClass;

} // namespace llvm

namespace llvm {
namespace snippy {

class GeneratorContext;
class RootRegPoolWrapper;

#ifdef LLVM_SNIPPY_ACCESS_MASKS
#error LLVM_SNIPPY_ACCESS_MASKS is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASKS                                               \
  LLVM_SNIPPY_ACCESS_MASK_DESC(None, 0b0000)                                   \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SR, 0b0001)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GR, 0b0010)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(R, 0b0011)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SW, 0b0100)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GW, 0b1000)                                     \
  LLVM_SNIPPY_ACCESS_MASK_DESC(W, 0b1100)                                      \
  LLVM_SNIPPY_ACCESS_MASK_DESC(SRW, 0b0101)                                    \
  LLVM_SNIPPY_ACCESS_MASK_DESC(GRW, 0b1010)                                    \
  LLVM_SNIPPY_ACCESS_MASK_DESC(RW, 0b1111)

// R - read.
// W - write.
// S - support instruction only.
// G - general instruction only.
enum class AccessMaskBit {
#ifdef LLVM_SNIPPY_ACCESS_MASK_DESC
#error LLVM_SNIPPY_ACCESS_MASK_DESC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASK_DESC(KEY, VALUE) KEY = VALUE,
  LLVM_SNIPPY_ACCESS_MASKS
#undef LLVM_SNIPPY_ACCESS_MASK_DESC
};

namespace detail {
template <typename T>
constexpr auto ConvertibleToConstMBBPtr =
    std::is_convertible_v<T, const MachineBasicBlock *>;
} // namespace detail

class LocalRegPool final {
  std::map<unsigned, AccessMaskBit> Reserved;

public:
  // Check if register is reserved in current pool.
  bool isReserved(unsigned Reg,
                  AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Append current reserved set.
  void addReserved(unsigned Reg, AccessMaskBit AccessMask = AccessMaskBit::RW);

  // Pick N random non-reserved registers in specified register class with
  // required access mask.
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class with
  // required access mask and filter.
  template <typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, Pred Filter,
                         unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class with required
  // access mask.
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class with required
  // access mask and filter.
  template <typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  void print(raw_ostream &OS) const;

  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
};

class RegPool {
  std::map<const MachineFunction *, LocalRegPool, MIRComp> ReservedForFunction;
  std::map<const MachineBasicBlock *, LocalRegPool, MIRComp> ReservedForBlock;
  LocalRegPool ReservedForApp;

  // Check if register is reserved for a specified function.
  bool isReservedForMF(unsigned Reg, const MachineFunction &MF,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified loop.
  bool isReservedForML(unsigned Reg, const MachineLoop &ML,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified block.
  bool isReservedForMBB(unsigned Reg, const MachineBasicBlock &MBB,
                        AccessMaskBit AccessMask = AccessMaskBit::RW) const;

public:
  // Check if register is reserved for app.
  bool isReserved(unsigned Reg,
                  AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified function and all parents.
  bool isReserved(unsigned Reg, const MachineFunction &MF,
                  AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified loop and all parents.
  bool isReserved(unsigned Reg, const MachineLoop &ML,
                  AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified block and all parents.
  bool isReserved(unsigned Reg, const MachineBasicBlock &MBB,
                  AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Check if register is reserved for a specified range of blocks and all
  // parents.
  template <typename R>
  std::enable_if_t<
      detail::ConvertibleToConstMBBPtr<decltype(*std::declval<R>().begin())>,
      bool>
  isReserved(unsigned Reg, R &&MBBs,
             AccessMaskBit AccessMask = AccessMaskBit::RW) const {
    return any_of(MBBs,
                  [=](auto *MBB) { return isReserved(Reg, *MBB, AccessMask); });
  }

  // Pick random non-reserved register in specified register class with required
  // access mask.
  template <typename R>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, R &&MBBs,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class with required
  // access mask.
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       const MachineBasicBlock &MBB,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class in a loop
  // with required access mask.
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, const MachineLoop &ML,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class in a block
  // with required access mask and filter.
  template <typename R, typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, R &&MBBs, Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class in a block
  // with required access mask and filter.
  template <typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       const MachineBasicBlock &MBB, Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick random non-reserved register in specified register class in a loop
  // with required access mask and filter.
  template <typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, const MachineLoop &ML,
                       Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a loop
  // with required access mask.
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a block
  // with required access mask.
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a block
  // with required access mask.
  template <typename R>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, R &&MBBs,
                         unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename R>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, R &&MBBs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a loop
  // with required access mask and filter.
  template <typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         unsigned NumRegs, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a block
  // with required access mask and filter.
  template <typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, unsigned NumRegs,
                         Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Pick N random non-reserved registers in specified register class in a block
  // with required access mask and filter.
  template <typename R, typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, R &&MBBs,
                         unsigned NumRegs, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename Pred>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename Pred>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename R, typename Pred>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, R &&MBBs, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass,
                           const MachineFunction &MF,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass,
                           const MachineBasicBlock &MBB,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename R>
  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass, R &&MBBs,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  // Append reserved set application-wide.
  void addReserved(unsigned Reg, AccessMaskBit AccessMask = AccessMaskBit::RW);
  // Append reserved set for a function.
  void addReserved(const MachineFunction &MF, unsigned Reg,
                   AccessMaskBit AccessMask = AccessMaskBit::RW);
  // Append reserved set for a loop.
  void addReserved(const MachineLoop &ML, unsigned Reg,
                   AccessMaskBit AccessMask = AccessMaskBit::RW);
  // Append reserved set for a block.
  void addReserved(const MachineBasicBlock &MBB, unsigned Reg,
                   AccessMaskBit AccessMask = AccessMaskBit::RW);

  void print(raw_ostream &OS) const {
    OS << "App RegPool: ";
    OS.indent(2);
    ReservedForApp.print(OS);
    OS << "MF RegPools:\n";
    for (auto &[MF, LRP] : ReservedForFunction) {
      OS.indent(2);
      OS << "[" << MF->getName() << "]: ";
      LRP.print(OS);
    }
    OS << "MBB RegPools:\n";
    for (auto &[MBB, LRP] : ReservedForBlock) {
      OS.indent(2);
      OS << "[" << MBB->getFullName() << "]: ";
      LRP.print(OS);
    }
  }

  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
};

class RegPoolWrapper final {
  std::vector<RegPool> &Pools;

public:
  RegPoolWrapper(std::vector<RegPool> &Pools) : Pools(Pools) {
    Pools.emplace_back();
  }

  RegPoolWrapper(const RegPoolWrapper &Other) = delete;
  RegPoolWrapper(RegPoolWrapper &&Other) = delete;

  RegPoolWrapper &operator=(const RegPoolWrapper &Other) = delete;
  RegPoolWrapper &operator=(RegPoolWrapper &&Other) = delete;

  // Create and push new wrapper
  RegPoolWrapper push() { return {Pools}; }

  // Pop wrapper on delete
  ~RegPoolWrapper() noexcept { Pools.pop_back(); }

  void reset() { Pools.back() = RegPool{}; }

  template <typename... ArgsTys> bool isReserved(ArgsTys &&...Args) const {
    return any_of(Pools, [&](auto &Pool) {
      return Pool.isReserved(std::forward<ArgsTys>(Args)...);
    });
  }

  template <typename... IsReservedTys>
  unsigned getNumAvailable(const MCRegisterClass &RegClass,
                           IsReservedTys &&...Args) const;

  template <typename Pred, typename... IsReservedTys,
            std::enable_if_t<std::is_invocable_v<Pred, Register>, bool> = true>
  unsigned getNumAvailable(const MCRegisterClass &RegClass, Pred Filter,
                           IsReservedTys &&...Args) const;

  template <typename... IsReservedTys>
  unsigned getNumAvailableInSet(ArrayRef<Register> RegisterSet,
                                IsReservedTys &&...Args) const;

  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       const MachineBasicBlock &MBB,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, const MachineLoop &ML,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass,
                       const MachineBasicBlock &MBB, Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename Pred>
  unsigned
  getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                       const MCRegisterClass &RegClass, const MachineLoop &ML,
                       Pred Filter,
                       AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, unsigned NumRegs,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         unsigned NumRegs, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename Pred>
  std::vector<unsigned>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, unsigned NumRegs,
                         Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename Pred>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass, const MachineLoop &ML,
                         Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <size_t N, typename Pred>
  std::array<unsigned, N>
  getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                         const MCRegisterClass &RegClass,
                         const MachineBasicBlock &MBB, Pred Filter,
                         AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass,
                           const MachineLoop &ML,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  std::vector<unsigned>
  getAllAvailableRegisters(const MCRegisterClass &RegClass,
                           const MachineBasicBlock &MBB,
                           AccessMaskBit AccessMask = AccessMaskBit::RW) const;

  template <typename... ArgsTys> void addReserved(ArgsTys &&...Args) {
    DEBUG_WITH_TYPE("snippy-regpool",
                    (dbgs() << "Before reservation: ", dump()));
    Pools.back().addReserved(std::forward<ArgsTys>(Args)...);
    DEBUG_WITH_TYPE("snippy-regpool",
                    (dbgs() << "After reservation: ", dump()));
  }

  void print(raw_ostream &OS) const {
    OS << "RegPoolWrapper's pools:\n";
    for (auto &&[Idx, Value] : enumerate(Pools)) {
      OS << "[" << Idx << "]:\n";
      Value.print(OS);
    }
  }

  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
};

template <typename Pred>
unsigned
LocalRegPool::getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                                   const MCRegisterClass &RegClass, Pred Filter,
                                   AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass,
                                       AccessMask);
}

template <typename Pred>
std::vector<unsigned> LocalRegPool::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, Pred Filter, unsigned NumRegs,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    AccessMask);
}

template <typename R>
unsigned
RegPool::getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                              const MCRegisterClass &RegClass, R &&MBBs,
                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, MBBs,
                                       AccessMask);
}

template <typename R, typename Pred>
unsigned
RegPool::getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                              const MCRegisterClass &RegClass, R &&MBBs,
                              Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass, MBBs,
                                       AccessMask);
}

template <typename Pred>
unsigned
RegPool::getAvailableRegister(const Twine &Desc, const MCRegisterInfo &RI,
                              const MCRegisterClass &RegClass,
                              const MachineBasicBlock &MBB, Pred Filter,
                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass, MBB,
                                       AccessMask);
}

template <typename Pred>
unsigned RegPool::getAvailableRegister(const Twine &Desc,
                                       const MCRegisterInfo &RI,
                                       const MCRegisterClass &RegClass,
                                       const MachineLoop &ML, Pred Filter,
                                       AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass, ML,
                                       AccessMask);
}

template <typename R>
std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass, R &&MBBs,
                                unsigned NumRegs,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass, MBBs,
                                    AccessMask);
}

template <typename R, typename Pred>
std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass, R &&MBBs,
                                unsigned NumRegs, Pred Filter,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    MBBs, AccessMask);
}

template <typename Pred>
std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineBasicBlock &MBB, unsigned NumRegs,
                                Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    MBB, AccessMask);
}

template <typename Pred>
std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineLoop &ML, unsigned NumRegs,
                                Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    ML, AccessMask);
}

template <size_t N, typename R>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass, R &&MBBs,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, RegClass, MBBs,
                                       AccessMask);
}

template <size_t N>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineBasicBlock &MBB,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, RegClass, MBB,
                                       AccessMask);
}

template <size_t N>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineLoop &ML,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, RegClass, ML,
                                       AccessMask);
}

template <size_t N, typename R, typename Pred>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass, R &&MBBs,
                                Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, std::move(Filter),
                                       RegClass, MBBs, AccessMask);
}

template <size_t N, typename Pred>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineBasicBlock &MBB, Pred Filter,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, std::move(Filter),
                                       RegClass, MBB, AccessMask);
}

template <size_t N, typename Pred>
std::array<unsigned, N>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineLoop &ML, Pred Filter,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, std::move(Filter),
                                       RegClass, ML, AccessMask);
}

template <typename... IsReservedTys>
unsigned RegPoolWrapper::getNumAvailable(const MCRegisterClass &RegClass,
                                         IsReservedTys &&...Args) const {
  return std::count_if(
      RegClass.begin(), RegClass.end(), [&Args..., this](auto Reg) {
        return !isReserved(Reg, std::forward<IsReservedTys>(Args)...);
      });
}

template <typename Pred, typename... IsReservedTys,
          std::enable_if_t<std::is_invocable_v<Pred, Register>, bool>>
unsigned RegPoolWrapper::getNumAvailable(const MCRegisterClass &RegClass,
                                         Pred Filter,
                                         IsReservedTys &&...Args) const {
  return std::count_if(RegClass.begin(), RegClass.end(), [&](auto Reg) {
    return !(isReserved(Reg, std::forward<IsReservedTys>(Args)...) ||
             std::invoke(Filter, Reg));
  });
}

template <typename... IsReservedTys>
unsigned RegPoolWrapper::getNumAvailableInSet(ArrayRef<Register> Registers,
                                              IsReservedTys &&...Args) const {
  return std::count_if(
      Registers.begin(), Registers.end(), [&Args..., this](auto Reg) {
        const auto *Pool = this; // silence erroneous warning
        return !Pool->isReserved(Reg, std::forward<IsReservedTys>(Args)...);
      });
}

template <typename Pred>
unsigned RegPoolWrapper::getAvailableRegister(const Twine &Desc,
                                              const MCRegisterInfo &RI,
                                              const MCRegisterClass &RegClass,
                                              const MachineBasicBlock &MBB,
                                              Pred Filter,
                                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass, MBB,
                                       AccessMask);
}

template <typename Pred>
unsigned RegPoolWrapper::getAvailableRegister(const Twine &Desc,
                                              const MCRegisterInfo &RI,
                                              const MCRegisterClass &RegClass,
                                              const MachineLoop &ML,
                                              Pred Filter,
                                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, Filter, RegClass, ML,
                                       AccessMask);
}

template <typename Pred>
std::vector<unsigned> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineBasicBlock &MBB,
    unsigned NumRegs, Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    MBB, AccessMask);
}

template <typename Pred>
std::vector<unsigned> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineLoop &ML, unsigned NumRegs,
    Pred Filter, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, Filter, NumRegs, RegClass,
                                    ML, AccessMask);
}

template <size_t N>
std::array<unsigned, N> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineBasicBlock &MBB,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, RegClass, MBB,
                                       AccessMask);
}

template <size_t N>
std::array<unsigned, N> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineLoop &ML,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, RegClass, ML,
                                       AccessMask);
}

template <size_t N, typename Pred>
std::array<unsigned, N> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineBasicBlock &MBB, Pred Filter,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, std::move(Filter),
                                       RegClass, MBB, AccessMask);
}

template <size_t N, typename Pred>
std::array<unsigned, N> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineLoop &ML, Pred Filter,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get<N>(Desc, RI, *this, std::move(Filter),
                                       RegClass, ML, AccessMask);
}
} // namespace snippy
} // namespace llvm
