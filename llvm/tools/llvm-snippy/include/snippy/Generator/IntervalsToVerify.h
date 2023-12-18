//===-- IntervalsToVerify.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {

class MCDisassembler;
class StringRef;
class raw_ostream;

namespace snippy {

class IntervalsToVerify {
public:
  // Closed interval [First, Last]
  struct Interval {
    uint64_t First;
    uint64_t Last;
  };

private:
  IntervalsToVerify(std::vector<Interval> Vec) : ToVerify(std::move(Vec)) {}

public:
  static Expected<IntervalsToVerify>
  createFromObject(MCDisassembler &D, StringRef ObjectBytes,
                   StringRef EntryPointName, uint64_t SectionVMA,
                   size_t PrologueInstrCnt, size_t EpilogueInstrCnt);

  void dumpAsYaml(raw_ostream &OS);
  Error dumpAsYaml(StringRef Filename);

  std::vector<Interval> ToVerify;
};

} // namespace snippy
} // namespace llvm
