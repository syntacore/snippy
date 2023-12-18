//===-- ImmediateHistogram.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/OpcodeHistogram.h"

#include <unordered_map>

namespace llvm {
namespace snippy {
class OpcodeToImmHistSequenceMap final {
  std::unordered_map<unsigned, ImmHistOpcodeSettings> Data;

public:
  OpcodeToImmHistSequenceMap(const ImmediateHistogramRegEx &ImmHist,
                             const OpcodeHistogram &OpcHist,
                             const OpcodeCache &OpCC);

  OpcodeToImmHistSequenceMap() = default;

  const ImmHistOpcodeSettings &
  getConfigForOpcode(unsigned Opc, const OpcodeCache &OpCC) const {
    assert(Data.count(Opc) && "Opcode was not found in immediate histogram");
    return Data.at(Opc);
  }
};

} // namespace snippy
} // namespace llvm
