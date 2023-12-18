//===-- RegisterPool.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/RegisterPool.h"
#include "snippy/Generator/RegisterPoolImpl.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"

#include <functional>

#define DEBUG_TYPE "snippy-regpool"

namespace llvm {
namespace snippy {

namespace {

template <typename T, typename U, typename F>
T invokeWithCast(F &&Func, U LHS, U RHS) {
  return std::invoke(Func, static_cast<T>(LHS), static_cast<T>(RHS));
}

template <typename CompT, typename T, typename F>
void invokeWithCastAndAssign(T &LHS, T RHS, F &&Func) {
  LHS = static_cast<T>(invokeWithCast<CompT>(Func, LHS, RHS));
}

bool checkAccessMask(AccessMaskBit Mask, AccessMaskBit ToCheck) {
  return invokeWithCast<unsigned>(std::bit_and<unsigned>(), Mask, ToCheck);
}

void addToAccessMask(AccessMaskBit Mask, AccessMaskBit ToAdd) {
  invokeWithCastAndAssign<unsigned>(Mask, ToAdd, std::bit_or<unsigned>());
}

auto accessMaskToStr(AccessMaskBit Mask) {
#ifdef LLVM_SNIPPY_ACCESS_MASK_DESC
#error LLVM_SNIPPY_ACCESS_MASK_DESC is already defined
#endif
#define LLVM_SNIPPY_ACCESS_MASK_DESC(KEY, VALUE)                               \
  case AccessMaskBit::KEY:                                                     \
    return #KEY;

  switch (Mask) {
    LLVM_SNIPPY_ACCESS_MASKS
#undef LLVM_SNIPPY_ACCESS_MASK_DESC
  default:
    return "Invalid";
  }
}

} // namespace

void LocalRegPool::print(raw_ostream &OS) const {
  for (auto [Reg, Access] : Reserved)
    OS << Reg << ":" << accessMaskToStr(Access) << " ";
  OS << "\n";
}

bool RegPool::isReserved(unsigned Reg, AccessMaskBit AccessMask) const {
  return ReservedForApp.isReserved(Reg, AccessMask);
}

bool RegPool::isReservedForMF(unsigned Reg, const MachineFunction &MF,
                              AccessMaskBit AccessMask) const {
  auto Pos = ReservedForFunction.find(&MF);
  auto End = ReservedForFunction.end();
  return Pos != End && Pos->second.isReserved(Reg, AccessMask);
}

bool RegPool::isReservedForML(unsigned Reg, const MachineLoop &ML,
                              AccessMaskBit AccessMask) const {
  return any_of(ML.blocks(), [this, Reg, AccessMask](auto *MBB) {
    return isReservedForMBB(Reg, *MBB, AccessMask);
  });
}

bool RegPool::isReservedForMBB(unsigned Reg, const MachineBasicBlock &MBB,
                               AccessMaskBit AccessMask) const {
  auto Pos = ReservedForBlock.find(&MBB);
  auto End = ReservedForBlock.end();
  return Pos != End && Pos->second.isReserved(Reg, AccessMask);
}

bool RegPool::isReserved(unsigned Reg, const MachineFunction &MF,
                         AccessMaskBit AccessMask) const {
  return isReserved(Reg, AccessMask) || isReservedForMF(Reg, MF, AccessMask);
}

bool RegPool::isReserved(unsigned Reg, const MachineLoop &ML,
                         AccessMaskBit AccessMask) const {
  const auto *MF = (*ML.block_begin())->getParent();
  return isReserved(Reg, *MF, AccessMask) ||
         isReservedForML(Reg, ML, AccessMask);
}

bool RegPool::isReserved(unsigned Reg, const MachineBasicBlock &MBB,
                         AccessMaskBit AccessMask) const {
  const auto *MF = MBB.getParent();
  return isReserved(Reg, *MF, AccessMask) ||
         isReservedForMBB(Reg, MBB, AccessMask);
}

unsigned RegPool::getAvailableRegister(const Twine &Desc,
                                       const MCRegisterInfo &RI,
                                       const MCRegisterClass &RegClass,
                                       const MachineLoop &ML,
                                       AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, ML,
                                       AccessMask);
}

unsigned RegPool::getAvailableRegister(const Twine &Desc,
                                       const MCRegisterInfo &RI,
                                       const MCRegisterClass &RegClass,
                                       const MachineBasicBlock &MBB,
                                       AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, MBB,
                                       AccessMask);
}

std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineLoop &ML, unsigned NumRegs,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass, ML,
                                    AccessMask);
}

std::vector<unsigned>
RegPool::getNAvailableRegisters(const Twine &Desc, const MCRegisterInfo &RI,
                                const MCRegisterClass &RegClass,
                                const MachineBasicBlock &MBB, unsigned NumRegs,
                                AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass, MBB,
                                    AccessMask);
}

std::vector<unsigned>
RegPool::getAllAvailableRegisters(const MCRegisterClass &RegClass,
                                  AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getAll(*this, RegClass, AccessMask);
}

std::vector<unsigned>
RegPool::getAllAvailableRegisters(const MCRegisterClass &RegClass,
                                  const MachineFunction &MF,
                                  AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getAll(*this, RegClass, MF, AccessMask);
}

std::vector<unsigned>
RegPool::getAllAvailableRegisters(const MCRegisterClass &RegClass,
                                  const MachineBasicBlock &MBB,
                                  AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getAll(*this, RegClass, MBB, AccessMask);
}

void RegPool::addReserved(unsigned Reg, AccessMaskBit AccessMask) {
  ReservedForApp.addReserved(Reg, AccessMask);
  LLVM_DEBUG(
      dbgs() << "Reserved reg [" << Reg << "] for app with access mask "
             << (checkAccessMask(AccessMask, AccessMaskBit::R) ? "R" : "")
             << (checkAccessMask(AccessMask, AccessMaskBit::W) ? "W" : "")
             << "\n");
}

void RegPool::addReserved(const MachineFunction &MF, unsigned Reg,
                          AccessMaskBit AccessMask) {
  ReservedForFunction[&MF].addReserved(Reg, AccessMask);
  LLVM_DEBUG(
      dbgs() << "Reserved reg [" << Reg << "] for MF '" << MF.getName()
             << "' with access mask "
             << (checkAccessMask(AccessMask, AccessMaskBit::R) ? "R" : "")
             << (checkAccessMask(AccessMask, AccessMaskBit::W) ? "W" : "")
             << "\n");
}

void RegPool::addReserved(const MachineLoop &ML, unsigned Reg,
                          AccessMaskBit AccessMask) {
  for_each(ML.blocks(), [this, Reg, AccessMask](auto *MBB) {
    addReserved(*MBB, Reg, AccessMask);
  });
  LLVM_DEBUG(
      dbgs() << "Reserved reg [" << Reg << "] for ML with access mask "
             << (checkAccessMask(AccessMask, AccessMaskBit::R) ? "R" : "")
             << (checkAccessMask(AccessMask, AccessMaskBit::W) ? "W" : "")
             << "\n");
}

void RegPool::addReserved(const MachineBasicBlock &MBB, unsigned Reg,
                          AccessMaskBit AccessMask) {
  ReservedForBlock[&MBB].addReserved(Reg, AccessMask);
  LLVM_DEBUG(
      dbgs() << "Reserved reg [" << Reg << "] for MBB '" << MBB.getFullName()
             << "' with access mask "
             << (checkAccessMask(AccessMask, AccessMaskBit::R) ? "R" : "")
             << (checkAccessMask(AccessMask, AccessMaskBit::W) ? "W" : "")
             << "\n");
}

bool LocalRegPool::isReserved(unsigned Reg, AccessMaskBit AccessMask) const {
  return Reserved.count(Reg) && checkAccessMask(Reserved.at(Reg), AccessMask);
}

void LocalRegPool::addReserved(unsigned Reg, AccessMaskBit AccessMask) {
  auto Entry = Reserved.try_emplace(Reg, AccessMask);
  // Amend access mask restriction if entry exists.
  if (!Entry.second)
    addToAccessMask(Entry.first->second, AccessMask);
}

unsigned LocalRegPool::getAvailableRegister(const Twine &Desc,
                                            const MCRegisterInfo &RI,
                                            const MCRegisterClass &RegClass,
                                            AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, AccessMask);
}

std::vector<unsigned> LocalRegPool::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, unsigned NumRegs,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass,
                                    AccessMask);
}

unsigned RegPoolWrapper::getAvailableRegister(const Twine &Desc,
                                              const MCRegisterInfo &RI,
                                              const MCRegisterClass &RegClass,
                                              const MachineBasicBlock &MBB,
                                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, MBB,
                                       AccessMask);
}

unsigned RegPoolWrapper::getAvailableRegister(const Twine &Desc,
                                              const MCRegisterInfo &RI,
                                              const MCRegisterClass &RegClass,
                                              const MachineLoop &ML,
                                              AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getOne(Desc, RI, *this, RegClass, ML,
                                       AccessMask);
}

std::vector<unsigned> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineLoop &ML, unsigned NumRegs,
    AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass, ML,
                                    AccessMask);
}

std::vector<unsigned> RegPoolWrapper::getNAvailableRegisters(
    const Twine &Desc, const MCRegisterInfo &RI,
    const MCRegisterClass &RegClass, const MachineBasicBlock &MBB,
    unsigned NumRegs, AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::get(Desc, RI, *this, NumRegs, RegClass, MBB,
                                    AccessMask);
}

std::vector<unsigned>
RegPoolWrapper::getAllAvailableRegisters(const MCRegisterClass &RegClass,
                                         const MachineLoop &ML,
                                         AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getAll(*this, RegClass, ML, AccessMask);
}

std::vector<unsigned>
RegPoolWrapper::getAllAvailableRegisters(const MCRegisterClass &RegClass,
                                         const MachineBasicBlock &MBB,
                                         AccessMaskBit AccessMask) const {
  return AvailableRegisterImpl::getAll(*this, RegClass, MBB, AccessMask);
}

} // namespace snippy
} // namespace llvm
