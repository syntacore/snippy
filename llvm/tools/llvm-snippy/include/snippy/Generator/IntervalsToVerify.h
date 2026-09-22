//===-- IntervalsToVerify.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INTERVALSTOVERIFY_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INTERVALSTOVERIFY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
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
  // Functions declared as external in the call graph are emitted with stub
  // bodies solely for running the snippet on a model; they are overridden by
  // user-provided code in the final image, so their addresses in the object
  // must not be verified.
  static Expected<IntervalsToVerify>
  createFromObject(MCDisassembler &D, StringRef ObjectBytes,
                   StringRef EntryPointName, uint64_t SectionVMA,
                   size_t PrologueInstrCnt, size_t EpilogueInstrCnt,
                   ArrayRef<std::string> ExternalFnNames);

  void dumpAsYaml(raw_ostream &OS);
  Error dumpAsYaml(StringRef Filename);

  std::vector<Interval> ToVerify;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INTERVALSTOVERIFY_H
