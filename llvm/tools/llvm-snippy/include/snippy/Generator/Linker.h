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

#include "snippy/Config/Config.h"
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

  struct OutputSection final {
    explicit OutputSection(const SectionDesc &Desc);
    SectionDesc Desc;
    // almost final section name (before mangling)
    std::string Name;
  };

  struct InputSection final {
    std::string Name;
  };

  struct SectionEntry {
    // parameters of the section in the linked file
    // (before mangling)
    OutputSection OutputSection;
    // Sections from initial object file(s) that are
    // placed in OutputSection during linking.
    std::vector<InputSection> InputSections;
  };

  class LinkedSections final : private std::vector<SectionEntry> {
    auto getSectionImpl(StringRef Name) const {
      return std::find_if(begin(), end(), [Name](const auto &E) {
        return E.OutputSection.Name == Name;
      });
    }

    auto getSectionImpl(StringRef Name) {
      return std::find_if(begin(), end(), [Name](const auto &E) {
        return E.OutputSection.Name == Name;
      });
    }

    auto getOutputSectionImpl(const SectionDesc &Desc) const {
      return std::find_if(begin(), end(), [&Desc](const auto &E) {
        return E.OutputSection.Desc == Desc;
      });
    }

    auto getOutputSectionImpl(const SectionDesc &Desc) {
      return std::find_if(begin(), end(), [&Desc](const auto &E) {
        return E.OutputSection.Desc == Desc;
      });
    }

  public:
    using typename vector::value_type;

    using vector::back;
    using vector::begin;
    using vector::empty;
    using vector::end;
    using vector::front;
    using vector::rbegin;
    using vector::rend;
    using vector::size;

    using vector::push_back;

    bool contains(const std::string &Name) const {
      return getSectionImpl(Name) != end();
    }

    std::string getOutputNameForDesc(const SectionDesc &Desc) const;

    bool hasOutputSectionFor(StringRef InSectName) const;
    const OutputSection &getOutputSectionFor(StringRef InSectName) const;

    auto &getSection(StringRef Name) {
      auto Section = getSectionImpl(Name);
      assert(Section != end());
      return *Section;
    }

    auto &getSection(StringRef Name) const {
      auto Section = getSectionImpl(Name);
      assert(Section != end());
      return *Section;
    }

    void addInputSectionFor(const SectionDesc &OutDesc, StringRef InSectName);
  };

  explicit Linker(LLVMContext &Ctx, const SectionsDescriptions &Sects,
                  StringRef MangleName = "");

  // Sections are sorted by their VMA value (not ID value!)
  auto &sections() { return Sections; }

  auto &sections() const { return Sections; }

  void setStartPC(uint64_t Addr) { StartPC = Addr; }
  auto getStartPC() const { return StartPC; }

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

  // Generates linker script for external usage.
  std::string generateLinkerScript() const;

  // Generates image using internally generated linker script.
  std::string run(ObjectFilesList ObjectFilesToLink, bool Relocatable,
                  bool DisableRelaxations = false) const;

  // Returns start and end address of minimal memory region that covers
  // all sections provided in layout file.
  auto memoryRegion() const { return MemoryRegion; }

  // Returns name of symbol that should be inserted before last generated
  // instruction.
  static StringRef getExitSymbolName();

private:
  std::vector<std::string> collectPhdrInfo() const;
  std::string createLinkerScript(bool Export) const;
  void calculateMemoryRegion();

  std::string MangleName;
  std::pair<size_t, size_t> MemoryRegion;
  LinkedSections Sections;
  std::optional<uint64_t> StartPC;
};

} // namespace snippy
} // namespace llvm
