#ifndef LLVM_TOOLS_SNIPPY_RISCV_TRACE_OBSERVER_H
#define LLVM_TOOLS_SNIPPY_RISCV_TRACE_OBSERVER_H

//===-- RISCVTraceObserver.h---------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/RISCVConverterSNTF.h"

namespace llvm {
namespace snippy {

// RISCVTraceObserver accepts callbacks from the model to record the state
// changes. It then reports all logs for this step to the RISCVConverterSNTF to
// convert the trace element to SNTF format.
class RISCVTraceObserver final : public Observer {
public:
  std::vector<std::unique_ptr<RISCVConverterSNTF>> Converters;

private:
  SmallVector<RegisterLog> RegLogs;
  SmallVector<MemoryLog> MemLogs;

public:
  void xregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void fregUpdateNotification(unsigned RegID, RegisterType Value) override;
  void vregUpdateNotification(unsigned RegID, ArrayRef<char> Data) override;
  void PCUpdateNotification(ProgramCounterType PC) override;
  void memUpdateNotification(MemoryAddressType Addr, const char *Data,
                             size_t Size) override;
  void memReadNotification(MemoryAddressType Addr, const char *Data,
                           size_t Size) override;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_RISCV_TRACE_OBSERVER_H
