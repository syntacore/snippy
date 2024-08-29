//===-- Linker.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Linker is used to place snippet sections to appropriate load addresses
/// to comply with user provided layout.
///
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/MemoryScheme.h"

#include "llvm/ADT/SmallVector.h"

namespace llvm {
namespace snippy {

class SnippyModule;
using ObjectFilesList = std::vector<SmallString<32>>;

class Linker final {
public:
  constexpr static const char *kDefaultTextSectionName = ".text";
  constexpr static const char *kDefaultRODataSectionName = ".rodata";

  struct OutputSectionT {
    explicit OutputSectionT(const SectionDesc &Desc);
    SectionDesc Desc;
    // almost final section name (before mangling)
    std::string Name;
  };

  struct SectionEntry {
    // parameters of the section in the linked file
    // (before mangling)
    OutputSectionT OutputSection;
    // Sections from initial object file(s) that are
    // placed in OutputSection during linking.
    std::vector<std::string> InputSections;
  };

  explicit Linker(LLVMContext &Ctx, const SectionsDescriptions &Sects,
                  StringRef MangleName = "");

  // Checks if Linker has mapped section from object file to its
  // destination in final elf image.
  bool hasOutputSectionFor(StringRef sectionName) const;

  // Gets mapped section info.
  OutputSectionT getOutputSectionFor(StringRef sectionName) const;

  std::string getOutputNameForDesc(const SectionDesc &Desc) const;

  // Sets input section for specified SectionDesc
  //  in order to avoid (NOLOAD) specifier in the linker script
  void addInputSectionForDescr(SectionDesc OutputSectionDesc,
                               StringRef InpSectName);

  // Sections are sorted by their VMA value (not ID value!)
  auto &sections() const { return Sections; }

  auto getMaxSectionID() const {
    auto Unnamed = llvm::make_filter_range(
        Sections,
        +[](const SectionEntry &S) { return !S.OutputSection.Desc.isNamed(); });
    auto IDs = llvm::map_range(
        Unnamed, [](auto &S) { return S.OutputSection.Desc.getNumber(); });
    auto MaxId = std::max_element(IDs.begin(), IDs.end());
    return MaxId == IDs.end() ? 0 : *MaxId;
  }

  // Returns mangled name for supposed section with name SectionName.
  // This is the name that section will have in resulted elf image.
  std::string getMangledName(StringRef SectionName) const;

  std::string getMangledFunctionName(StringRef FuncName) const;

  // Add section mapping. If InputSectionName is empty, final section in elf
  // will be marked as NOLOAD.
  void addSection(const SectionDesc &Section, StringRef InputSectionName = "");

  // Generates linker script for external usage.
  std::string generateLinkerScript() const;

  // Generates image using internally generated linker script.
  std::string run(ObjectFilesList ObjectFilesToLink, bool Relocatable) const;

  // Returns start and end address of minimal memory region that covers
  // all sections provided in layout file.
  auto memoryRegion() const { return MemoryRegion; }

  // Returns name of symbol that should be inserted before last generated
  // instruction.
  static StringRef GetExitSymbolName();

private:
  std::vector<std::string> collectPhdrInfo() const;
  std::string createLinkerScript(bool Export) const;
  void calculateMemoryRegion();

  std::string MangleName;
  std::pair<size_t, size_t> MemoryRegion;
  SmallVector<SectionEntry, 4> Sections;
};

} // namespace snippy
} // namespace llvm
