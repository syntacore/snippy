//===-- Simulator.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/Simulator.h"

#include "llvm/Support/YAMLTraits.h"

namespace llvm {

void yaml::MappingTraits<snippy::SimulationConfig>::mapping(
    yaml::IO &IO, snippy::SimulationConfig &Cfg) {
  IO.mapRequired("prog_start", Cfg.ProgStart);
  IO.mapRequired("prog_size", Cfg.ProgSize);
  IO.mapRequired("rom_start", Cfg.RomStart);
  IO.mapRequired("rom_size", Cfg.RomSize);
  IO.mapRequired("ram_start", Cfg.RamStart);
  IO.mapRequired("ram_size", Cfg.RamSize);
  IO.mapOptional("prog_section", Cfg.ProgSectionName);
  IO.mapOptional("rom_section", Cfg.RomSectionName);
  IO.mapOptional("trace_log", Cfg.TraceLogPath);
}

} // namespace llvm
