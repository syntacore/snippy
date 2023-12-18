//===-- Transactions.h -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Observer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

namespace llvm {
namespace snippy {

class TransactionStack final : public Observer {
public:
  using RegIdToValueType = DenseMap<unsigned, RegisterType>;
  using VRegIdToValueType = DenseMap<unsigned, VectorRegisterType>;
  using AddrToDataType = DenseMap<MemoryAddressType, char>;
  struct OpenTransaction {
    AddrToDataType Mem;
    // FIXME: Transactions mechanism is expected to be a target-independent, so
    // we shouldn't use any dedicated types. Looks like one more
    // target-dependent layer of abstraction is required.
    RegIdToValueType XRegs;
    RegIdToValueType FRegs;
    VRegIdToValueType VRegs;
    std::optional<ProgramCounterType> PC;
  };

  using MemSnapshotType =
      std::unordered_map<MemoryAddressType, std::vector<char>>;
  using RegSnapshotType = DenseMap<unsigned, RegisterType>;
  using VRegSnapshotType = DenseMap<unsigned, VectorRegisterType>;
  struct Snapshot {
    MemSnapshotType Mem;
    RegSnapshotType XRegs;
    RegSnapshotType FRegs;
    VRegSnapshotType VRegs;
    ProgramCounterType PC;
  };

  bool empty() const;
  size_t size() const;
  void clearLastTransaction();
  void push();
  void pop();

  void memUpdateNotification(MemoryAddressType Addr, const char *Data,
                             size_t Size) override;
  void xregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void fregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void vregUpdateNotification(unsigned RegID, ArrayRef<char> Data) override;
  void PCUpdateNotification(ProgramCounterType PC) override;

  void addMemSnapshot(MemoryAddressType Addr, std::vector<char> Snapshot);
  void addXRegToSnapshot(unsigned RegID, RegisterType Value);
  void addFRegToSnapshot(unsigned RegID, RegisterType Value);
  void addVRegToSnapshot(unsigned RegID, VectorRegisterType Value);
  void addPCToSnapshot(ProgramCounterType PC);

  // get original values of the memory/registers changed by the last open
  // transaction.
  AddrToDataType getMemBeforeTransaction() const;
  RegIdToValueType getXRegsBeforeTransaction() const;
  RegIdToValueType getFRegsBeforeTransaction() const;
  VRegIdToValueType getVRegsBeforeTransaction() const;
  ProgramCounterType getPCBeforeTransaction() const;

  const AddrToDataType &getMemChangedByTransaction() const;
  const RegIdToValueType &getXRegsChangedByTransaction() const;
  const RegIdToValueType &getFRegsChangedByTransaction() const;
  const VRegIdToValueType &getVRegsChangedByTransaction() const;
  ProgramCounterType getPC() const;

  void print(raw_ostream &OS) const;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif

private:
  using TransactionsType = SmallVector<OpenTransaction, 8>;
  TransactionsType Transactions;
  Snapshot InitialSnapshot;

  char getMemPrevValue(MemoryAddressType Addr,
                       TransactionsType::const_reverse_iterator I) const;
  RegisterType
  getXRegPrevValue(unsigned RegID,
                   TransactionsType::const_reverse_iterator I) const;
  RegisterType
  getFRegPrevValue(unsigned RegID,
                   TransactionsType::const_reverse_iterator I) const;
  VectorRegisterType
  getVRegPrevValue(unsigned RegID,
                   TransactionsType::const_reverse_iterator I) const;
  ProgramCounterType
  getPCPrevValue(TransactionsType::const_reverse_iterator I) const;
};

} // namespace snippy
} // namespace llvm
