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
#include "llvm/Target/TargetLoweringObjectFile.h"

#include <cassert>
#include <vector>

#define DEBUG_TYPE "snippy-memory-manager"

namespace llvm::snippy {

namespace {

struct MemorySectionConfig {
  MemAddr Start = 0;
  MemAddr Size = 0;
  std::string Name;
};

MemorySectionConfig getProgSectionInfo(const Linker &L) {
  if (!L.hasOutputSectionFor(".text"))
    report_fatal_error("Incorrect list of sections: there are no suitable "
                       "RX section",
                       false);
  auto CodeSection = L.getOutputSectionFor(".text");
  auto ProgStart = CodeSection.Desc.VMA;
  auto ProgSize = CodeSection.Desc.Size;
  auto ProgSectionName = L.getMangledName(CodeSection.Name);

  LLVM_DEBUG(dbgs() << "ProgramStart: " << ProgStart << "\n";
             dbgs() << "ProgramSize: " << ProgSize << "\n");

  return {ProgStart, ProgSize, ProgSectionName};
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
  return {RamVMABegin, MaxRamVMAEnd - RamVMABegin};
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
  // get RX sections info...
  auto &&[ProgSectionStart, ProgSectionSize, ProgSectionName] =
      getProgSectionInfo(L);
  // get R sections info...
  auto &&[RomStart, RomSize, RomSectionName] = getRomInfo(L, ProgSectionStart);
  // get RW sections info...
  auto RamCfg = getRamInfo(L);

  return {/* ProgSectionStart */ ProgSectionStart,
          /* ProgSectionSize */ ProgSectionSize,
          /* ProgSectionName */ ProgSectionName,
          /* RomStart */ RomStart,
          /* RomSize */ RomSize,
          /* RomSectionName */ RomSectionName,
          /* RamStart */ RamCfg.Start,
          /* RamSize */ RamCfg.Size};
}

} // namespace llvm::snippy
