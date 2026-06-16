//===-- RISCVFeatureFlagsSNTF.cpp----------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/RISCVFeatureFlagsSNTF.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {

void yaml::MappingTraits<snippy::RISCVFeatureFlagsSNTF>::mapping(
    yaml::IO &IO, snippy::RISCVFeatureFlagsSNTF &SNTF) {
  IO.mapOptional("time", SNTF.EnableTime);
  IO.mapOptional("pc", SNTF.EnablePC);
  IO.mapOptional("instr-code", SNTF.EnableInstrCode);
  IO.mapOptional("next-pc", SNTF.EnableNextPC);
  IO.mapOptional("registers-changed", SNTF.EnableRegVals);
  IO.mapOptional("memory-accesses", SNTF.EnableMemAccesses);
}

} // namespace llvm
