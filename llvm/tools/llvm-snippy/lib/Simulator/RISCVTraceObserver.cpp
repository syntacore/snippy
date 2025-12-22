//===-- RISCVTraceObserver.cpp-------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/RISCVTraceObserver.h"

namespace llvm {
namespace snippy {

void RISCVTraceObserver::xregUpdateNotification(unsigned RegID,
                                                RegisterType Value) {
  RegLogs.emplace_back(RegisterLog{RegType::X, RegID, Value});
}

void RISCVTraceObserver::fregUpdateNotification(unsigned RegID,
                                                RegisterType Value) {
  RegLogs.emplace_back(RegisterLog{RegType::F, RegID, Value});
}

void RISCVTraceObserver::vregUpdateNotification(unsigned RegID,
                                                ArrayRef<char> Data) {
  auto DataSize = Data.size() * CHAR_BIT;
  auto VecData = VectorRegisterType(
      DataSize, ArrayRef<uint64_t>(
                    reinterpret_cast<const uint64_t *>(Data.data()), DataSize));
  RegLogs.emplace_back(RegisterLog{RegType::V, RegID, VecData});
}

void RISCVTraceObserver::memUpdateNotification(MemoryAddressType Addr,
                                               const char *Data, size_t Size) {
  auto DataSize = Size * CHAR_BIT;
  auto Value = APInt(
      DataSize,
      ArrayRef<uint64_t>(reinterpret_cast<const uint64_t *>(Data), DataSize));
  MemLogs.emplace_back(MemoryLog{MemoryLog::AccessType::W, Addr, Value});
}

void RISCVTraceObserver::PCUpdateNotification(ProgramCounterType PC) {
  for_each(Converters, [&](auto &Converter) {
    Converter->acceptRecordsAndShiftPC(PC, RegLogs, MemLogs);
  });
  RegLogs.clear();
  MemLogs.clear();
}

void RISCVTraceObserver::memReadNotification(MemoryAddressType Addr,
                                             const char *Data, size_t Size) {
  auto DataSize = Size * CHAR_BIT;
  auto Value = APInt(
      DataSize,
      ArrayRef<uint64_t>(reinterpret_cast<const uint64_t *>(Data), DataSize));
  MemLogs.emplace_back(MemoryLog{MemoryLog::AccessType::R, Addr, Value});
}

} // namespace snippy
} // namespace llvm
