//===-- MemoryManager.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/MemoryManager.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Regex.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

#include <cassert>
#include <vector>

#define DEBUG_TYPE "snippy-memory-manager"

namespace llvm::snippy {

namespace {

void fillProgSectionInfo(const Linker &L, MemoryConfig &Config) {
  for (auto &ExecSection : L.executionPath()) {
    if (ExecSection.InputSections.empty())
      continue;
    Config.ProgSections.emplace_back(
        ExecSection.OutputSection.Desc.VMA, ExecSection.OutputSection.Desc.Size,
        L.getMangledName(ExecSection.OutputSection.Name));
    LLVM_DEBUG(
        dbgs() << "ProgramStart: " << Config.ProgSections.back().Start << "\n";
        dbgs() << "ProgramSize: " << Config.ProgSections.back().Size << "\n");
  }

  if (Config.ProgSections.empty())
    report_fatal_error("Incorrect list of sections: no used RX sections found",
                       false);
}

MemorySectionConfig getRamInfo(const Linker &L) {
  auto IsRW = [](const Linker::SectionEntry &S) {
    auto &M = S.OutputSection.Desc.M;
    return M.R() && M.W() && !M.X();
  };
  const auto &Sections = L.sections();
  auto RWSectionIt = std::find_if(Sections.begin(), Sections.end(), IsRW);
  assert(RWSectionIt != Sections.end());

  auto RWSectionLast =
      std::find_if(Sections.rbegin(), Sections.rend(), IsRW).base();

  if (std::any_of(RWSectionIt, RWSectionLast, std::not_fn(IsRW)) ||
      (RWSectionIt != Sections.begin() && RWSectionLast != Sections.end()))
    report_fatal_error("Incorrect list of sections: all RW sections must go "
                       "either before all other sections or after them all",
                       false);

  const auto &OutputSectionDesc = RWSectionIt->OutputSection.Desc;
  auto RamVMABegin = OutputSectionDesc.VMA;
  auto MaxRamVMAEnd = RamVMABegin + OutputSectionDesc.Size;

  for (const auto &S : llvm::make_range(++RWSectionIt, RWSectionLast)) {
    RamVMABegin = std::min(RamVMABegin, S.OutputSection.Desc.VMA);
    MaxRamVMAEnd = std::max(MaxRamVMAEnd, S.OutputSection.Desc.VMA +
                                              S.OutputSection.Desc.Size);
  }
  assert(MaxRamVMAEnd >= RamVMABegin);
  return {RamVMABegin, MaxRamVMAEnd - RamVMABegin, ""};
}

MemorySectionConfig getRomInfo(const Linker &L, MemAddr ProgSectionStart) {
  MemAddr RomStart;
  MemAddr RomSize;
  std::string RomSectionName;
  if (L.hasOutputSectionFor(".rodata")) {
    const auto &ROMSection = L.getOutputSectionFor(".rodata");
    const auto &ROMSectionDesc = ROMSection.Desc;
    RomStart = ROMSectionDesc.VMA;
    RomSize = ROMSectionDesc.Size;
    RomSectionName = L.getMangledName(ROMSection.Name);
  } else {
    RomStart = ProgSectionStart;
    RomSize = 0;
  }

  LLVM_DEBUG(dbgs() << "ROM Start: " << RomStart << "\n";
             dbgs() << "ROM Size: " << RomSize << "\n";
             dbgs() << "ROM Section name: " << RomSectionName << "\n");

  return {RomStart, RomSize, RomSectionName};
}

} // namespace

MemoryConfig MemoryConfig::getMemoryConfig(const Linker &L) {
  MemoryConfig Config{};
  // get RX sections info...
  fillProgSectionInfo(L, Config);
  // get R sections info...
  Config.Rom = getRomInfo(L, Config.ProgSections.front().Start);
  // get RW sections info...
  Config.Ram = getRamInfo(L);

  return Config;
}

} // namespace llvm::snippy
