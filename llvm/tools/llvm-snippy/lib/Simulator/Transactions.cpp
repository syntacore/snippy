//===-- Transactions.cpp -----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/Transactions.h"

#include "llvm/ADT/STLExtras.h"

#define DEBUG_TYPE "snippy-transactions"

namespace llvm {
namespace snippy {

TransactionStack::AddrToDataType
TransactionStack::getMemBeforeTransaction() const {
  assert(!empty());
  AddrToDataType MemChange;
  for (auto [Addr, _] : Transactions.back().Mem)
    MemChange[Addr] = getMemPrevValue(Addr, Transactions.rbegin());
  return MemChange;
}

TransactionStack::RegIdToValueType
TransactionStack::getXRegsBeforeTransaction() const {
  assert(!empty());
  RegIdToValueType XRegsChange;
  for (auto [RegId, _] : Transactions.back().XRegs)
    XRegsChange[RegId] = getXRegPrevValue(RegId, Transactions.rbegin());
  return XRegsChange;
}

TransactionStack::RegIdToValueType
TransactionStack::getFRegsBeforeTransaction() const {
  assert(!empty());
  RegIdToValueType FRegsChange;
  for (auto [RegId, _] : Transactions.back().FRegs)
    FRegsChange[RegId] = getFRegPrevValue(RegId, Transactions.rbegin());
  return FRegsChange;
}

TransactionStack::VRegIdToValueType
TransactionStack::getVRegsBeforeTransaction() const {
  assert(!empty());
  VRegIdToValueType VRegsChange;
  for (auto [RegId, _] : Transactions.back().VRegs)
    VRegsChange[RegId] = getVRegPrevValue(RegId, Transactions.rbegin());
  return VRegsChange;
}

ProgramCounterType TransactionStack::getPCBeforeTransaction() const {
  assert(!empty());
  return getPCPrevValue(Transactions.rbegin());
}

const TransactionStack::AddrToDataType &
TransactionStack::getMemChangedByTransaction() const {
  assert(!empty());
  return Transactions.back().Mem;
}

const TransactionStack::RegIdToValueType &
TransactionStack::getXRegsChangedByTransaction() const {
  assert(!empty());
  return Transactions.back().XRegs;
}

const TransactionStack::RegIdToValueType &
TransactionStack::getFRegsChangedByTransaction() const {
  assert(!empty());
  return Transactions.back().FRegs;
}

const TransactionStack::VRegIdToValueType &
TransactionStack::getVRegsChangedByTransaction() const {
  assert(!empty());
  return Transactions.back().VRegs;
}

ProgramCounterType TransactionStack::getPC() const {
  assert(!empty());
  const auto &PC = Transactions.back().PC;
  return PC ? *PC : getPCPrevValue(Transactions.rbegin());
}

char TransactionStack::getMemPrevValue(
    MemoryAddressType Addr, TransactionsType::const_reverse_iterator I) const {
  assert(!empty());
  assert(I != Transactions.rend());
  auto PrevEntryIt =
      std::find_if(std::next(I), Transactions.rend(),
                   [Addr](const auto &P) { return P.Mem.count(Addr); });
  if (PrevEntryIt != Transactions.rend())
    return PrevEntryIt->Mem.lookup(Addr);
  auto &MemSnapshot = InitialSnapshot.Mem;
  auto MemSnapshotIt = std::find_if(
      MemSnapshot.begin(), MemSnapshot.end(), [Addr](const auto &S) {
        auto Start = S.first;
        auto Size = S.second.size();
        return Start <= Addr && (Addr < Start + Size || Start + Size == 0ull);
      });
  assert(MemSnapshotIt != MemSnapshot.end() &&
         "Memory snapshot must contain whole memory.");
  return MemSnapshotIt->second[Addr - MemSnapshotIt->first];
}

RegisterType TransactionStack::getXRegPrevValue(
    unsigned RegID, TransactionsType::const_reverse_iterator I) const {
  assert(!empty());
  assert(I != Transactions.rend());
  assert(InitialSnapshot.XRegs.count(RegID));
  auto PrevEntryIt =
      std::find_if(std::next(I), Transactions.rend(),
                   [RegID](const auto &P) { return P.XRegs.count(RegID); });
  if (PrevEntryIt != Transactions.rend())
    return PrevEntryIt->XRegs.lookup(RegID);
  return InitialSnapshot.XRegs.lookup(RegID);
}

RegisterType TransactionStack::getFRegPrevValue(
    unsigned RegID, TransactionsType::const_reverse_iterator I) const {
  assert(!empty());
  assert(I != Transactions.rend());
  assert(InitialSnapshot.FRegs.count(RegID));
  auto PrevEntryIt =
      std::find_if(std::next(I), Transactions.rend(),
                   [RegID](const auto &P) { return P.FRegs.count(RegID); });
  if (PrevEntryIt != Transactions.rend())
    return PrevEntryIt->FRegs.lookup(RegID);
  return InitialSnapshot.FRegs.lookup(RegID);
}

VectorRegisterType TransactionStack::getVRegPrevValue(
    unsigned RegID, TransactionsType::const_reverse_iterator I) const {
  assert(!empty());
  assert(I != Transactions.rend());
  assert(InitialSnapshot.VRegs.count(RegID));
  auto PrevEntryIt =
      std::find_if(std::next(I), Transactions.rend(),
                   [RegID](const auto &P) { return P.VRegs.count(RegID); });
  if (PrevEntryIt != Transactions.rend())
    return PrevEntryIt->VRegs.lookup(RegID);
  return InitialSnapshot.VRegs.lookup(RegID);
}

ProgramCounterType TransactionStack::getPCPrevValue(
    TransactionsType::const_reverse_iterator I) const {
  assert(!empty());
  assert(I != Transactions.rend());
  auto PrevEntryIt =
      std::find_if(std::next(I), Transactions.rend(),
                   [](const auto &P) { return P.PC.has_value(); });
  if (PrevEntryIt != Transactions.rend())
    return PrevEntryIt->PC.value();
  return InitialSnapshot.PC;
}
void TransactionStack::addMemSnapshot(MemoryAddressType Addr,
                                      std::vector<char> Snapshot) {
  auto &MemSnapshot = InitialSnapshot.Mem;
  assert(MemSnapshot.count(Addr) == 0 &&
         "Attempt to add already existing memory snapshot.");
  MemSnapshot[Addr] = std::move(Snapshot);
}

void TransactionStack::addXRegToSnapshot(unsigned RegID, RegisterType Value) {
  auto &XRegsSnapshot = InitialSnapshot.XRegs;
  assert(XRegsSnapshot.count(RegID) == 0 &&
         "Attempt to add already existing xreg to snapshot.");
  XRegsSnapshot[RegID] = Value;
}

void TransactionStack::addFRegToSnapshot(unsigned RegID, RegisterType Value) {
  auto &FRegsSnapshot = InitialSnapshot.FRegs;
  assert(FRegsSnapshot.count(RegID) == 0 &&
         "Attempt to add already existing freg to snapshot.");
  FRegsSnapshot[RegID] = Value;
}

void TransactionStack::addVRegToSnapshot(unsigned RegID,
                                         VectorRegisterType Value) {
  auto &VRegsSnapshot = InitialSnapshot.VRegs;
  assert(VRegsSnapshot.count(RegID) == 0 &&
         "Attempt to add already existing vreg to snapshot.");
  VRegsSnapshot[RegID] = std::move(Value);
}

void TransactionStack::addPCToSnapshot(ProgramCounterType PC) {
  InitialSnapshot.PC = PC;
}

bool TransactionStack::empty() const { return Transactions.empty(); }

size_t TransactionStack::size() const { return Transactions.size(); }

void TransactionStack::clearLastTransaction() {
  assert(!empty());
  Transactions.back().PC.reset();
  Transactions.back().Mem.clear();
  Transactions.back().XRegs.clear();
  Transactions.back().FRegs.clear();
  Transactions.back().VRegs.clear();
  LLVM_DEBUG(dbgs() << "Clear last transaction\n");
}

void TransactionStack::push() {
  Transactions.emplace_back();
  LLVM_DEBUG(dbgs() << "Push new transaction\n");
}

void TransactionStack::pop() {
  assert(!empty());
  Transactions.pop_back();
  LLVM_DEBUG(dbgs() << "Pop transaction\n");
  if (!empty())
    return;
  InitialSnapshot.Mem.clear();
  InitialSnapshot.XRegs.clear();
  InitialSnapshot.FRegs.clear();
  InitialSnapshot.VRegs.clear();
}

void TransactionStack::memUpdateNotification(MemoryAddressType Addr,
                                             const char *Data, size_t Size) {
  if (empty())
    return;
  auto AddrEnd = Addr + Size;
  assert(
      any_of(
          InitialSnapshot.Mem,
          [Addr, AddrEnd](const auto &MemChunk) {
            auto MemBegin = MemChunk.first;
            auto MemEnd = MemBegin + MemChunk.second.size();
            bool Overlap = std::max(Addr, MemBegin) < std::min(AddrEnd, MemEnd);
            assert(
                (!Overlap || (MemBegin <= Addr && AddrEnd <= MemEnd)) &&
                "When overlap, incoming data must fit in one memory snapshot.");
            return Overlap;
          }) &&
      "Received memory update notification for the address absent in memory "
      "snapshot.");

  for (size_t i = 0; i < Size; ++i) {
    Transactions.back().Mem[Addr + i] = Data[i];
    LLVM_DEBUG(dbgs() << "Memory update notification: addr " << Addr + i
                      << ", data " << static_cast<int>(Data[i]) << "\n");
  }
}

void TransactionStack::xregUpdateNotification(unsigned RegID,
                                              RegisterType Value) {
  if (empty())
    return;
  Transactions.back().XRegs[RegID] = Value;
  LLVM_DEBUG(dbgs() << "xreg update notification: id " << RegID << ", data "
                    << Value << "\n");
}

void TransactionStack::fregUpdateNotification(unsigned RegID,
                                              RegisterType Value) {
  if (empty())
    return;
  Transactions.back().FRegs[RegID] = Value;
  LLVM_DEBUG(dbgs() << "freg update notification: id " << RegID << ", data "
                    << Value << "\n");
}

void TransactionStack::vregUpdateNotification(unsigned RegID,
                                              ArrayRef<char> Data) {
  if (empty())
    return;
  assert(!Data.empty());
  auto DataTypeSize = sizeof(Data[0]) * CHAR_BIT;
  APInt Value(Data.size() * DataTypeSize, 0);
  for (auto [Idx, Byte] : enumerate(Data))
    Value.insertBits(Byte, Idx * DataTypeSize, DataTypeSize);

  LLVM_DEBUG(dbgs() << "vreg update notification: id " << RegID << ", data "
                    << Value << "\n");
  Transactions.back().VRegs[RegID] = std::move(Value);
}

void TransactionStack::PCUpdateNotification(ProgramCounterType PC) {
  if (empty())
    return;
  Transactions.back().PC = PC;
  LLVM_DEBUG(dbgs() << "PC update notification: " << PC << "\n");
}

void TransactionStack::print(raw_ostream &OS) const {
  OS << "Open transactions stack(from the latest to earliest):\n";
  for (auto I = Transactions.rbegin(); I != Transactions.rend(); ++I) {
    OS << "  Open transaction #" << std::distance(I, Transactions.rend())
       << " {\n";
    if (I->PC)
      OS << "    PC change: (Old) " << *I->PC << " -> " << getPCPrevValue(I)
         << " (New)\n";
    for (auto [Addr, Value] : I->Mem)
      OS << "    Memory change at addr " << Addr << ": (Old) "
         << static_cast<int>(getMemPrevValue(Addr, I)) << " -> "
         << static_cast<int>(Value) << " (New)\n";
    for (auto [Id, Value] : I->XRegs)
      OS << "    XReg change at index " << Id << ": (Old) "
         << getXRegPrevValue(Id, I) << " -> " << Value << " (New)\n";
    for (auto [Id, Value] : I->FRegs)
      OS << "    FReg change at index " << Id << ": (Old) "
         << getFRegPrevValue(Id, I) << " -> " << Value << " (New)\n";
    for (const auto &[Id, Value] : I->VRegs)
      OS << "    VReg change at index " << Id << ": (Old) "
         << getVRegPrevValue(Id, I) << " -> " << Value << " (New)\n";
    OS << "  }\n";
  }
}

} // namespace snippy
} // namespace llvm
