//===-- SMCGram.cpp ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/SMCGram.h"
#include "snippy/Support/YAMLNumericRange.h"

#include "llvm/Support/YAMLTraits.h"
namespace llvm {
using namespace snippy;

template <> struct yaml::ScalarEnumerationTraits<SMCGram::SMCMode> {
  static void enumeration(IO &IO, SMCGram::SMCMode &K) {
    if (K != SMCGram::SMCMode::Immediate) {
      IO.setError("Currently smc-mode option supports only immediate mode");
      snippy::fatal("Failed to parse smcgram");
    }
    IO.enumCase(K, getSMCModeName<SMCGram::SMCMode::Immediate>().data(),
                SMCGram::SMCMode::Immediate);
  }
};

void yaml::MappingTraits<SMCGram>::mapping(yaml::IO &IO, SMCGram &SMC) {
  IO.mapOptional("smc-tgt-blocks", SMC.SMCTgtBlocksRatio);
  IO.mapOptional("smc-overwriters", SMC.SMCOverwriters);
  IO.mapOptional("smc-mode", SMC.Mode);
}

std::string yaml::MappingTraits<SMCGram>::validate(yaml::IO &IO, SMCGram &SMC) {
  if ((SMC.SMCTgtBlocksRatio < 0.0 || SMC.SMCTgtBlocksRatio > 1.0))
    return std::string("smc-tgt-blocks option expected to be >= 0 and <= 1");
  return std::string();
}

} // namespace llvm
