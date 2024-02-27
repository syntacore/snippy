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
  auto &ProgSection = Cfg.ProgSections.empty() ? Cfg.ProgSections.emplace_back()
                                               : Cfg.ProgSections.front();
  IO.mapRequired("prog_start", ProgSection.Start);
  IO.mapRequired("prog_size", ProgSection.Size);
  IO.mapRequired("rom_start", Cfg.RomStart);
  IO.mapRequired("rom_size", Cfg.RomSize);
  IO.mapRequired("ram_start", Cfg.RamStart);
  IO.mapRequired("ram_size", Cfg.RamSize);
  IO.mapOptional("prog_section", ProgSection.Name);
  IO.mapOptional("rom_section", Cfg.RomSectionName);
  IO.mapOptional("trace_log", Cfg.TraceLogPath);
}

} // namespace llvm
