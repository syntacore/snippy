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

void RISCVTraceObserver::csrUpdateNotification(unsigned RegID,
                                               RegisterType Value) {
  CSRLogs.emplace_back(RegisterLog{RegType::CSR, RegID, Value});
}

void RISCVTraceObserver::PCUpdateNotification(ProgramCounterType PC) {
  for_each(Converters, [&](auto &Converter) {
    Converter->acceptRecordsAndShiftPC(PC, RegLogs, CSRLogs, MemLogs);
  });
  RegLogs.clear();
  CSRLogs.clear();
  MemLogs.clear();
}

void RISCVTraceObserver::handleMemReadOrUpdateNotification(
    bool IsRead, MemoryAddressType Addr, const char *Data, size_t Size) {
  auto BitWidth = Size * CHAR_BIT;
  SmallVector<char, 64> Copy;
  Copy.resize(alignTo(Size, sizeof(uint64_t)));
  std::memcpy(Copy.data(), Data, Size);
  auto Value =
      APInt(BitWidth,
            ArrayRef<uint64_t>(reinterpret_cast<const uint64_t *>(Copy.data()),
                               Copy.size() / sizeof(uint64_t)));
  MemLogs.emplace_back(
      MemoryLog{IsRead ? MemoryLog::AccessType::R : MemoryLog::AccessType::W,
                Addr, Value});
}

void RISCVTraceObserver::memUpdateNotification(MemoryAddressType Addr,
                                               const char *Data, size_t Size) {
  handleMemReadOrUpdateNotification(/*IsRead=*/false, Addr, Data, Size);
}

void RISCVTraceObserver::memReadNotification(MemoryAddressType Addr,
                                             const char *Data, size_t Size) {
  handleMemReadOrUpdateNotification(/*IsRead=*/true, Addr, Data, Size);
}

} // namespace snippy
} // namespace llvm
