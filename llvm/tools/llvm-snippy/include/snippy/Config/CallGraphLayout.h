//===-- CallGraphLayout.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Classes that handle call graph layout. Layout is read from YAML.
///
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/LLVMState.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/MemoryBuffer.h"

namespace llvm {

namespace snippy {
struct CallGraphLayout {
  // Maximum layers in call graph (for now it is always DAG)
  unsigned MaxLayers;
  unsigned FunctionNumber;
  // Number of instructions in ancillary functions.
  unsigned InstrNumAncil;

  CallGraphLayout();

  void validate(LLVMContext &Ctx) const {
    if (MaxLayers == 0)
      fatal(Ctx, "Invalid number of function layers", "expected >=1");
    if (FunctionNumber == 0)
      fatal(Ctx, "Invalid number of functions", "expected >=1");
  }
};
} // namespace snippy

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::CallGraphLayout);

} // namespace llvm
