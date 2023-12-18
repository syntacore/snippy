//===-- SelfcheckObserver.h ---------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Observer.h"

#include "llvm/ADT/DenseMap.h"

// --- Selfcheck annotation description:
// After each PC update event Selfcheck Observer puts it to the vector
// when selfcheck observer detect a memory access it searches the related
// address besides selfcheck ones through the SelfcheckMap. If the searching is
// successful it considers a distance=N and takes an Nth from the end PC from
// PCs vector
//                           _________(distance)____________
//                         /                               /
// | PC_1 | PC_2 | reg = PC_3(prim instr) | ... | ... | store reg |

namespace llvm {
namespace snippy {

struct AnnotationPair {
  std::string Address;
  std::string PC;

  AnnotationPair(MemoryAddressType Addr = 0, ProgramCounterType PCVal = 0);
};

using AnnotationList = std::vector<AnnotationPair>;

class SelfcheckObserver final : public Observer {
  // Contain First selfcheck stores addresses and a distance (in 'steps' between
  // related primary and store instructions
  DenseMap<MemoryAddressType, MemoryAddressType> SelfcheckMap;
  std::vector<ProgramCounterType> PCs;
  AnnotationList SelfcheckAnnotation;

public:
  template <typename Iter>
  SelfcheckObserver(Iter Begin, Iter End, ProgramCounterType StartPC)
      : SelfcheckMap(Begin, End), PCs{StartPC} {}

  void memUpdateNotification(MemoryAddressType Addr, const char *Data,
                             size_t Size) override;

  void PCUpdateNotification(ProgramCounterType PC) override {
    PCs.push_back(PC);
  }

  auto &getSelfcheckAnnotation() { return SelfcheckAnnotation; }

  void dumpAsYaml(StringRef Path);
};

} // namespace snippy
} // namespace llvm
