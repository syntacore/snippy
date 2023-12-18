//===-- CallGraphLayout.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Config/CallGraphLayout.h"
#include "snippy/Support/Options.h"

#include "llvm/Support/YAMLTraits.h"

namespace llvm {

void yaml::MappingTraits<snippy::CallGraphLayout>::mapping(
    yaml::IO &IO, snippy::CallGraphLayout &Info) {
  IO.mapOptional("function-layers", Info.MaxLayers);
  IO.mapOptional("function-number", Info.FunctionNumber);
  IO.mapOptional("num-instr-ancil", Info.InstrNumAncil);
}

namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<unsigned>
    FunctionLayers("function-layers",
                   cl::desc("number of layers in call graph"), cl::cat(Options),
                   cl::init(1));

static snippy::opt<unsigned>
    FunctionNumberOpt("function-number",
                      cl::desc("number of functions generated in programm."),
                      cl::cat(Options), cl::init(1));

static snippy::opt<unsigned>
    NumInstrAncil("num-instr-ancil",
                  cl::desc("number of instructions in ancillary functions"),
                  cl::cat(Options), cl::init(10));

CallGraphLayout::CallGraphLayout()
    : MaxLayers(FunctionLayers), FunctionNumber(FunctionNumberOpt),
      InstrNumAncil(NumInstrAncil) {}

} // namespace snippy
} // namespace llvm
