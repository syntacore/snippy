//===-- GeneratorContext.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Generator/GenerationRequest.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/MemoryManager.h"
#include "snippy/Generator/PluginMemAccSampler.h"
#include "snippy/Generator/RandomMemAccSampler.h"
#include "snippy/Simulator/SelfcheckObserver.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"

#include <set>
#include <vector>

#define DEBUG_TYPE "snippy-gen-context"

namespace llvm {
namespace snippy {

template class GenResultT<SelfCheckMap>;

extern cl::OptionCategory Options;

static snippy::opt<size_t> StackSize("stack-size",
                                     cl::desc("size of snippy stack space"),
                                     cl::cat(Options), cl::init(0u));

static snippy::opt<bool> ForceEmitLinkerScript(
    "force-emit-linker-script",
    cl::desc("Snippy will emit linker script even if linker not being run "),
    cl::cat(Options), cl::init(false));

static snippy::opt<bool>
    DumpRandMemAccesses("dump-rand-mem-accesses",
                        cl::desc("Dump random memory accesses"),
                        cl::cat(Options), cl::Hidden);

static snippy::opt<std::string> MemAddrGeneratorInfoFile(
    "address-plugin-info-file",
    cl::desc("File with info for addresses generator. "
             "Use =None if plugin doesn't need additional info."
             "(=None - default value)"),
    cl::value_desc("filename"), cl::cat(Options), cl::init("None"));

static snippy::opt<std::string>
    MemAddrGeneratorFile("address-generator-plugin",
                         cl::desc("Plugin for custom addreses generation."
                                  "Use =None to generate addresses "
                                  "with build-in randomizer."
                                  "(=None - default value)"),
                         cl::value_desc("filename"), cl::cat(Options),
                         cl::init("None"));

static std::string getMemPlugin() {
  if (MemAddrGeneratorFile == "None")
    return {""};
  return MemAddrGeneratorFile;
}

static std::string getMemPluginInfo() {
  auto FileName = std::string{MemAddrGeneratorInfoFile};
  if (MemAddrGeneratorInfoFile == "None")
    FileName = "";
  if (!FileName.empty() && getMemPlugin().empty())
    report_fatal_error("Addresses generator plugin info file "
                       "may be used only with " +
                           Twine(MemAddrGeneratorFile.ArgStr) + "Option",
                       false);
  return FileName;
}

GeneratorContext::~GeneratorContext() {}

namespace {

template <typename AccessPred>
std::optional<MemAddr> findPlaceForNewSection(SectionsDescriptions &Sections,
                                              AccessPred CustomModePred,
                                              size_t SectionSize,
                                              size_t SectionAlignment = 1) {
  assert(!Sections.empty());
  auto SectionBegin =
      std::find_if(Sections.begin(), Sections.end(), CustomModePred);
  assert(SectionBegin != Sections.end());

  auto SectionEnd =
      std::find_if(Sections.rbegin(), Sections.rend(), CustomModePred).base();

  // Try to emplace before all sections.
  if (SectionBegin == Sections.begin()) {
    const auto &Desc = *SectionBegin;
    if (Desc.VMA >= SectionSize) {
      auto Unaligned = Desc.VMA - SectionSize;
      return alignDown(Unaligned, SectionAlignment);
    }
  }

  // Try to emplace after all sections.
  if (SectionEnd == Sections.end()) {
    const auto &Desc = *std::prev(SectionEnd);
    auto Unaligned = Desc.VMA + Desc.Size;
    auto Aligned = alignTo(Unaligned, SectionAlignment);
    if (std::numeric_limits<size_t>::max() - Aligned >= SectionSize) {
      return Aligned;
    }
  }

  // Finally, try to find place among sections.
  auto InsertPosBegin =
      SectionBegin == Sections.begin() ? SectionBegin : std::prev(SectionBegin);
  auto InsertPosEnd =
      SectionEnd == Sections.end() ? SectionEnd : std::next(SectionEnd);

  auto FoundPlace = std::adjacent_find(
      InsertPosBegin, InsertPosEnd,
      [SectionSize, SectionAlignment](const auto &Sec1, const auto &Sec2) {
        const auto &Sec1Desc = Sec1;
        const auto &Sec2Desc = Sec2;
        auto Sec1End = Sec1Desc.VMA + Sec1Desc.Size;
        assert(Sec2Desc.VMA >= Sec1End);
        auto EndAligned = alignTo(Sec1End, SectionAlignment);
        if (EndAligned > Sec2Desc.VMA)
          return false;
        return Sec2Desc.VMA - EndAligned >= SectionSize;
      });

  if (FoundPlace != InsertPosEnd) {
    const auto &Desc = *FoundPlace;
    return alignTo(Desc.VMA + Desc.Size, SectionAlignment);
  } else
    return {};
}

SectionDesc allocateRWSection(SectionsDescriptions &Sections,
                              size_t SectionSize, StringRef Name,
                              size_t Alignment = 1) {
  auto IsRW = [](const SectionDesc &SE) {
    auto M = SE.M;
    return M.R() && M.W() && !M.X();
  };
  auto SecVMA = findPlaceForNewSection(Sections, IsRW, SectionSize, Alignment);
  if (!SecVMA)
    snippy::fatal(
        formatv("Failed to emplace selfcheck section: Could not find {0}"
                " bytes of consecutive free space outside of "
                "sections specified in layout.",
                SectionSize));

  auto SecVMAValue = SecVMA.value();
  auto SCSection =
      SectionDesc{Name, SecVMAValue, SectionSize, SecVMAValue, "RW"};
  return SCSection;
}
} // namespace

void GeneratorContext::diagnoseSelfcheckSection(size_t MinSize) const {
  auto &State = getProgramContext().getLLVMState();
  auto &SelfcheckSection = getProgramContext().getSelfcheckSection();
  auto M = SelfcheckSection.M;
  if (!(M.R() && M.W() && !M.X()))
    snippy::fatal(State.getCtx(), "Wrong layout file",
                  "\"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section must be RW");
  auto &SelfcheckSectionSize = SelfcheckSection.Size;
  if (SelfcheckSectionSize < MinSize)
    snippy::fatal(
        State.getCtx(),
        "Cannot use \"" + Twine(SectionsDescriptions::SelfcheckSectionName) +
            "\" section from layout",
        "it does not fit selfcheck data (" + Twine(MinSize) + " bytes)");
  if ((SelfcheckSectionSize !=
       alignTo(SelfcheckSectionSize, SnippyProgramContext::getPageSize())) ||
      SelfcheckSection.VMA !=
          alignTo(SelfcheckSection.VMA, SnippyProgramContext::getPageSize()))
    snippy::fatal(State.getCtx(),
                  "Cannot use \"" +
                      Twine(SectionsDescriptions::SelfcheckSectionName) +
                      "\" section from layout",
                  "it has unaligned memory settings");
}

namespace {

size_t getMinimumSelfcheckSize(const GeneratorSettings &Settings) {
  assert(Settings.TrackingConfig.SelfCheckPeriod);

  size_t BlockSize = 2 * SnippyProgramContext::getSCStride();
  // Note: There are cases when we have some problems for accurate calculating
  // of selcheck section size.
  //       Consequently it can potentially cause overflow of selfcheck
  //       section, So it's better to provide selfcheck section in Layout
  //       explicitly
  return alignTo(Settings.InstrsGenerationConfig.NumInstrs.value_or(0) *
                     BlockSize / Settings.TrackingConfig.SelfCheckPeriod,
                 SnippyProgramContext::getPageSize());
}

void amendSelfcheckSection(LLVMState &State, const GeneratorSettings &Settings,
                           SectionsDescriptions &Amended) {
  if (!Settings.TrackingConfig.SelfCheckPeriod)
    return;

  auto AlignSize = getMinimumSelfcheckSize(Settings);
  if (Amended.hasSection(SectionsDescriptions::SelfcheckSectionName))
    return;
  snippy::warn(
      WarningName::InconsistentOptions, State.getCtx(),
      "Implicit selfcheck section use is discouraged",
      "please, provide \"selfcheck\" section description in layout file");
  Amended.push_back(allocateRWSection(
      Amended, AlignSize, SectionsDescriptions::SelfcheckSectionName));
  return;
}

void amendStackSection(LLVMState &State, const GeneratorSettings &Settings,
                       SectionsDescriptions &Amended) {

  auto &SnippyTgt = State.getSnippyTarget();
  auto SP = Settings.RegistersConfig.StackPointer;
  auto Align = SnippyTgt.getSpillAlignmentInBytes(SP, State);

  if (Amended.hasSection(SectionsDescriptions::StackSectionName))
    return;

  if (StackSize.getValue()) {
    // Manually place stack section when only stack size is provided.
    Amended.push_back(allocateRWSection(Amended, StackSize.getValue(),
                                        SectionsDescriptions::StackSectionName,
                                        Align));
  } else if (Settings.LinkerConfig.ExternalStack) {
    // Implicitly allocate section when external stack is enabled but no
    // specific stack section info provided.
    assert(SectionsDescriptions::ImplicitSectionSize % Align == 0 &&
           "Wrong estimated stack size alignment.");
    Amended.push_back(
        allocateRWSection(Amended, SectionsDescriptions::ImplicitSectionSize,
                          SectionsDescriptions::StackSectionName, Align));
  }
}

void amendUtilitySection(LLVMState &State, const GeneratorSettings &Settings,
                         SectionsDescriptions &Amended) {
  // Don't do anything if section is not needed.
  if (Settings.RegistersConfig.SpilledToMem.empty())
    return;

  if (Amended.hasSection(SectionsDescriptions::UtilitySectionName))
    return;

  if (!Amended.hasSection(SectionsDescriptions::StackSectionName))
    return;
  // Try to use up some of a stack space.

  auto &Ctx = State.getCtx();
  auto Size = 2 * Settings.RegistersConfig.SpilledToMem.size() *
              State.getSnippyTarget().getAddrRegLen(State.getTargetMachine()) /
              CHAR_BIT;
  auto &Stack = Amended.getSection(SectionsDescriptions::StackSectionName);
  auto UtilitySection = SectionDesc(SectionsDescriptions::UtilitySectionName);
  UtilitySection.VMA = std::exchange(Stack.VMA, Stack.VMA + Size);
  UtilitySection.LMA = std::exchange(Stack.LMA, Stack.LMA + Size);
  UtilitySection.Size = Size;
  if (Stack.Size <= Size) {
    auto &Ctx = State.getCtx();
    snippy::fatal(
        Ctx, "Stack section is too small",
        "stack cannot be used for internal purposes. Either provide \"" +
            Twine(SectionsDescriptions::UtilitySectionName) +
            "\" section or increase \"" +
            Twine(SectionsDescriptions::StackSectionName) +
            "\" section size. (stack size: " + Twine(Stack.Size) +
            ", required size: " + Twine(Size) + ")");
  }
  Stack.Size -= Size;
  UtilitySection.M = Stack.M;
  Amended.push_back(UtilitySection);
  snippy::notice(WarningName::NotAWarning, Ctx,
                 "No \"" + Twine(SectionsDescriptions::UtilitySectionName) +
                     "\" section was provided",
                 "Using part of \"" +
                     Twine(SectionsDescriptions::StackSectionName) +
                     "\" section for internal purposes instead");
}

} // namespace

SectionsDescriptions
GeneratorSettings::getCompleteSectionList(LLVMState &State) const {
  auto CompleteList = Cfg.Sections;
  amendSelfcheckSection(State, *this, CompleteList);
  amendStackSection(State, *this, CompleteList);
  amendUtilitySection(State, *this, CompleteList);
  return CompleteList;
}

GeneratorContext::GeneratorContext(SnippyProgramContext &ProgContext,
                                   GeneratorSettings &Settings)
    : ProgContext(ProgContext), GenSettings(&Settings), MemAccSampler([&] {
        std::vector<SectionDesc> RWSections;
        llvm::copy_if(Settings.Cfg.Sections, std::back_inserter(RWSections),
                      [](auto &Desc) { return Desc.M.W() && Desc.M.R(); });
        auto &MS = Settings.Cfg.MS;
        auto &TM = ProgContext.getLLVMState().getTargetMachine();
        auto Alignment =
            TM.createDataLayout().getPointerABIAlignment(/* Address Space */ 0);
        auto BaseAccesses = llvm::map_range(
            MS.BaseAccesses,
            [](const auto &A) -> const MemoryAccess & { return *A; });
        auto RandSampler = std::make_unique<RandomMemoryAccessSampler>(
            RWSections.begin(), RWSections.end(), BaseAccesses.begin(),
            BaseAccesses.end(), Alignment.value(), MS.Restricted);
        if (DumpRandMemAccesses)
          RandSampler->dump();
        auto PluginSampler = std::make_unique<PluginMemoryAccessSampler>(
            MemorySchemePluginWrapper{getMemPlugin(), getMemPluginInfo()});
        std::vector<std::unique_ptr<IMemoryAccessSampler>> Samplers;
        Samplers.emplace_back(std::move(PluginSampler));
        Samplers.emplace_back(std::move(RandSampler));
        return TopLevelMemoryAccessSampler(Samplers.begin(), Samplers.end());
      }()) {
  auto &State = ProgContext.getLLVMState();
  auto &SnippyTgt = State.getSnippyTarget();
  auto &Ctx = State.getCtx();
  auto &OpCC = ProgContext.getOpcodeCache();
  if (GenSettings->hasCallInstrs(OpCC, SnippyTgt)) {
    const auto &RI = State.getRegInfo();
    auto RA = RI.getRARegister();
    if (ProgContext.getRegisterPool().isReserved(RA))
      snippy::fatal(State.getCtx(),
                    "Cannot generate requested call instructions",
                    "return address register is explicitly reserved.");
  }
  auto SP = getProgramContext().getStackPointer();
  if (std::any_of(GenSettings->RegistersConfig.SpilledToStack.begin(),
                  GenSettings->RegistersConfig.SpilledToStack.end(),
                  [SP](auto Reg) { return Reg == SP; }))
    snippy::fatal("Stack pointer cannot be spilled. Remove it from "
                  "spill register list.");

  if (!getProgramContext().hasStackSection()) {
    if (!GenSettings->RegistersConfig.SpilledToStack.empty())
      snippy::fatal(Ctx, "Cannot spill requested registers",
                    "no stack space allocated.");

    if (GenSettings->hasCallInstrs(OpCC, SnippyTgt) &&
        GenSettings->Cfg.CGLayout.MaxLayers > 1u)
      snippy::fatal(
          State.getCtx(), "Cannot generate requested call instructions",
          "layout allows calls with depth>=1 but stack space is not provided.");
  }

  if (getProgramContext().hasExternalStack()) {
    if (GenSettings->ModelPluginConfig.RunOnModel)
      snippy::fatal(Ctx, "Cannot run snippet on model",
                    "external stack was enabled.");
    if (GenSettings->Cfg.Sections.hasSection(
            SectionsDescriptions::StackSectionName)) {
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "Section 'stack' will not be used",
                   "external stack was enabled.");
    }

    if (StackSize.getValue())
      snippy::warn(WarningName::InconsistentOptions, Ctx,
                   "--" + Twine(StackSize.ArgStr) + " option ignored",
                   "external stack was enabled.");
  }

  if (GenSettings->Cfg.Sections.hasSection(
          SectionsDescriptions::SelfcheckSectionName) &&
      Settings.TrackingConfig.SelfCheckPeriod)
    diagnoseSelfcheckSection(getMinimumSelfcheckSize(*GenSettings));
  ProgContext.createTargetContext(Settings);
}

} // namespace snippy
} // namespace llvm
