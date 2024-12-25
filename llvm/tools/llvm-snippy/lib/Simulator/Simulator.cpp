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

namespace {

struct RegionKey {
  const char *Name;
  const char *KeyStart;
  const char *KeySize;
};

// legacy named region mapping info.
constexpr std::array<RegionKey, 3> RegionKeys{
// clang-format off
#ifdef SNIPPY_REGION_KEY
#error "SNIPPY_REGION_KEY" should not be defined here
#endif
#define SNIPPY_REGION_KEY(X)                                                   \
  RegionKey { X, X "_start", X "_size" }
    SNIPPY_REGION_KEY("prog"), SNIPPY_REGION_KEY("rom"),
    SNIPPY_REGION_KEY("ram"),
#undef SNIPPY_REGION_KEY
    // clang-format on
};

} // namespace

void yaml::MappingTraits<snippy::SimulationConfig::Section>::mapping(
    yaml::IO &IO, snippy::SimulationConfig::Section &S) {
  IO.mapRequired("start", S.Start);
  IO.mapRequired("size", S.Size);
  IO.mapRequired("name", S.Name);
}

void yaml::MappingTraits<snippy::SimulationConfig>::mapping(
    yaml::IO &IO, snippy::SimulationConfig &Cfg) {
  if (!IO.outputting()) {
    std::optional<std::vector<snippy::SimulationConfig::Section>> RegionsOpt;
    IO.mapOptional("memory", RegionsOpt);
    if (RegionsOpt) {
      Cfg.MemoryRegions = *RegionsOpt;
    } else {
      // For backward compatibility
      Cfg.MemoryRegions.clear();

      llvm::transform(RegionKeys, std::back_inserter(Cfg.MemoryRegions),
                      [&](auto &&MappingInfo) {
                        snippy::SimulationConfig::Section LegacySection{};
                        IO.mapRequired(MappingInfo.KeyStart,
                                       LegacySection.Start);
                        IO.mapRequired(MappingInfo.KeySize, LegacySection.Size);
                        LegacySection.Name = MappingInfo.Name;
                        return LegacySection;
                      });
      std::string Dummy;
      IO.mapOptional("prog_section", Dummy);
      IO.mapOptional("rom_section", Dummy);
    }
  } else {
    std::array<decltype(Cfg.MemoryRegions)::iterator, 3> Regions;
    llvm::transform(RegionKeys, Regions.begin(), [&](auto &&Keys) {
      return llvm::find_if(Cfg.MemoryRegions, [&Keys](auto &&Region) {
        return Region.Name == Keys.Name;
      });
    });

    if (llvm::all_of(Regions, [&](auto &&It) {
          return It != Cfg.MemoryRegions.end();
        })) {
      // legacy format
      for (auto &&[Keys, RegionIt] : llvm::zip(RegionKeys, Regions)) {
        IO.mapRequired(Keys.KeyStart, RegionIt->Start);
        IO.mapRequired(Keys.KeySize, RegionIt->Size);
      }
    } else {
      // new format
      IO.mapRequired("memory", Cfg.MemoryRegions);
    }
  }
  IO.mapOptional("trace_log", Cfg.TraceLogPath);
}

} // namespace llvm
