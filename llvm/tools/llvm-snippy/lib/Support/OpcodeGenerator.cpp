//===-- OpcodeGenerator.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/OpcodeGenerator.h"
#include "snippy/Support/Utils.h"

namespace llvm {
namespace snippy {

void DefaultOpcodeGenerator::print(llvm::raw_ostream &OS) const {
  OS << "OpcodeGen:\n";
  auto Prob = getProbabilities();
  for (const auto &[Opcode, P] : Prob)
    OS << "     Opcode: " << Opcode << ": " << floatToString(P, 3) << "\n";
}

} // namespace snippy
} // namespace llvm
