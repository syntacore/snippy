//===-- Config.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/Branchegram.h"
#include "snippy/Config/BurstGram.h"
#include "snippy/Config/CallGraphLayout.h"
#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Config/FunctionDescriptions.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/PluginWrapper.h"
#include "snippy/Support/YAMLUtils.h"
#include "snippy/Target/TargetConfigIface.h"

namespace llvm {
namespace snippy {

class SnippyTarget;

class Config final {
public:
  std::vector<std::string> Includes;
  MemoryScheme MS;
  SectionsDescriptions Sections;
  OpcodeHistogram Histogram;
  Branchegram Branches;
  BurstGram Burst;
  ImmediateHistogram ImmHistogram;
  CallGraphLayout CGLayout;
  std::optional<FunctionDescs> FuncDescs;

  std::unique_ptr<PluginManager> PluginManagerImpl;
  std::unique_ptr<TargetConfigInterface> TargetConfig;

  Config(const SnippyTarget &Tgt, StringRef PluginFilename,
         StringRef PluginInfoFilename, OpcodeCache OpCC, bool ParseWithPlugin,
         LLVMContext &Ctx, ArrayRef<std::string> IncludedFiles);

  // FIXME: this should return OpcGenHolder
  std::unique_ptr<DefaultOpcodeGenerator> createDefaultOpcodeGenerator() const {
    return std::make_unique<DefaultOpcodeGenerator>(Histogram.begin(),
                                                    Histogram.end());
  }

  double getBurstOpcodesWeight() const {
    assert(Burst.Data.has_value());
    auto BurstOpcodes = Burst.Data.value().getAllBurstOpcodes();
    return Histogram.getOpcodesWeight([&BurstOpcodes](unsigned Opcode) {
      return BurstOpcodes.count(Opcode);
    });
  }

  // Create opcode generator for only data flow instructions excluding ones
  // which are in burst groups
  OpcGenHolder createDFOpcodeGenerator(const OpcodeCache &OpCC,
                                       std::function<bool(unsigned)> OpcMask,
                                       ArrayRef<OpcodeHistogramEntry> Overrides,
                                       bool MustHavePrimaryInstrs) const {
    // TODO: we should have an option to re-scale Override set
    // proportionally to the weight of deleted elements
    assert(Burst.Data.has_value());
    auto BurstOpcodes = Burst.Data.value().getAllBurstOpcodes();
    std::map<unsigned, double> DFHistogram;
    std::copy_if(Histogram.begin(), Histogram.end(),
                 std::inserter(DFHistogram, DFHistogram.end()),
                 [&OpCC, &OpcMask, &BurstOpcodes](const auto &Hist) {
                   auto *Desc = OpCC.desc(Hist.first);
                   assert(Desc);
                   return Desc->isBranch() == false && OpcMask(Hist.first) &&
                          !BurstOpcodes.count(Hist.first);
                 });
    if (MustHavePrimaryInstrs && DFHistogram.size() == 0)
      report_fatal_error(
          "We can not create any primary instruction in this context.\nUsually "
          "this may happen when in some context snippy can not find any "
          "instruction that could be created in current context.\nTry to "
          "increase instruction number by one or add more instructions to "
          "histogram.",
          false);

    for (const auto &Entry : Overrides)
      if (!Entry.deactivated())
        DFHistogram[Entry.Opcode] = Entry.Weight;
    if (PluginManagerImpl->pluginHasBeenLoaded())
      return PluginManagerImpl->createPlugin(DFHistogram.begin(),
                                             DFHistogram.end());
    return std::make_unique<DefaultOpcodeGenerator>(DFHistogram.begin(),
                                                    DFHistogram.end());
  }

  // Create opcode generator for only control flow instructions
  OpcGenHolder createCFOpcodeGenerator(const OpcodeCache &OpCC) const {
    std::map<unsigned, double> CFHistogram;
    std::copy_if(Histogram.begin(), Histogram.end(),
                 std::inserter(CFHistogram, CFHistogram.end()),
                 [&OpCC](const auto &Hist) {
                   auto *Desc = OpCC.desc(Hist.first);
                   assert(Desc);
                   return Desc->isBranch();
                 });
    if (PluginManagerImpl->pluginHasBeenLoaded())
      return PluginManagerImpl->createPlugin(CFHistogram.begin(),
                                             CFHistogram.end());
    return std::make_unique<DefaultOpcodeGenerator>(CFHistogram.begin(),
                                                    CFHistogram.end());
  }

  void dump(raw_ostream &OS, const ConfigIOContext &Ctx) const;
};

class IncludePreprocessor final {
public:
  // NOTE: Lifetime of FileName field is the same as the
  // lifetime of the enclosing class.
  // Care needs to be taken that this StringRef does not dangle.
  struct LineID final {
    StringRef FileName;
    unsigned N;
  };

private:
  std::string Text;
  std::vector<LineID> Lines;
  SmallSet<std::string, 8> IncludedFiles;

public:
  IncludePreprocessor(StringRef Filename,
                      const std::vector<std::string> &IncludeDirs,
                      LLVMContext &Ctx);

  void mergeFile(StringRef FileName, StringRef Contents);
  LineID getCorrespondingLineID(unsigned GlobalID) const & {
    assert(GlobalID > 0 && GlobalID <= Lines.size());
    return Lines[GlobalID - 1];
  }

  StringRef getPreprocessed() const & { return Text; }
  auto getIncludes() const & {
    return llvm::make_range(IncludedFiles.begin(), IncludedFiles.end());
  }
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::Config);
} // namespace llvm
