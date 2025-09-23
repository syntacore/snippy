//===--- RISCVInstructionEnumerator.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVInstructionEnumerator.h"
#include "CommandLineOpts.h"
#include "FeatureFilter.h"
#include "Plugins.h"
#include "RISCVExtensionCLParser.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/RISCVISAUtils.h"
#include "llvm/Support/WithColor.h"

#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "RISCVInstrEnums.inc"

LLVM_IE_TARGET_DEFINE(RISCVInstructionEnumerator)

using namespace llvm;
using namespace llvm_ie;

cl::opt<std::vector<RISCVExtensionCLEntry>, false, RISCVExtensionParser>
    ExtensionOpt("riscv-ext",
                 cl::desc("Specify list of enabled RISC-V extensions"));
cl::alias ExtensionOptAlias("e", cl::desc("Alias for -riscv-ext"),
                            cl::aliasopt(ExtensionOpt));
cl::opt<bool> RV32Opt("rv32", cl::desc("Only 32-bit instructions"),
                      cl::cat(opts::FeatureOptionsCategory));
cl::opt<bool> RV64Opt("rv64", cl::desc("Only 64-bit instructions"),
                      cl::cat(opts::FeatureOptionsCategory));

static std::vector<llvm::StringRef>
getAllImpliedExtensions(llvm::StringRef Extension) {
  std::vector<llvm::StringRef> ImpliedExtensions{};
  llvm::copy(ImpliedExts.at(Extension), std::back_inserter(ImpliedExtensions));

  std::vector<llvm::StringRef> AllImpliedExtensions{};
  while (!ImpliedExtensions.empty()) {
    llvm::copy(ImpliedExtensions, std::back_inserter(AllImpliedExtensions));

    std::vector<llvm::StringRef> NewImpliedExtensions{};
    for (llvm::StringRef Ext : ImpliedExtensions)
      llvm::copy(ImpliedExts.at(Ext), std::back_inserter(NewImpliedExtensions));

    ImpliedExtensions = std::move(NewImpliedExtensions);
  }

  return AllImpliedExtensions;
}

/// This function finds extension's implied extensions.
/// \param [in] Extension
///     Extension name.
/// \return
///     Vector consists of the extension itself and its implied extensions
///     (ExtensionGroup).
static std::vector<llvm::StringRef>
getExtensionGroup(llvm::StringRef Extension) {
  if (!llvm::is_contained(SupportedExtensions, Extension)) {
    WithColor::warning(errs())
        << "unknown RISC-V extension: " << Extension << "\n";
    return {};
  }

  std::vector<llvm::StringRef> ExtensionGroup{Extension};
  llvm::copy(getAllImpliedExtensions(Extension),
             std::back_inserter(ExtensionGroup));

  return ExtensionGroup;
}

static std::set<llvm::StringRef>
getInstructionsFromExtension(llvm::StringRef Extension) {
  if (Extension == "all")
    return AllRISCVInstructions;

  std::vector<llvm::StringRef> ExtensionGroup = getExtensionGroup(Extension);
  if (ExtensionGroup.empty())
    return {};

  std::vector<InstructionInfo> InstrInfos;
  llvm::copy_if(InstructionTable, std::back_inserter(InstrInfos),
                [&ExtensionGroup](auto &&InstrInfo) {
                  return llvm::any_of(
                      InstrInfo.second, [&ExtensionGroup](auto &&Extension) {
                        return llvm::is_contained(ExtensionGroup, Extension);
                      });
                });

  std::set<llvm::StringRef> Instrs{};
  llvm::transform(
      InstrInfos, std::inserter(Instrs, Instrs.begin()),
      [](const auto &Instr) -> llvm::StringRef { return Instr.first; });
  return Instrs;
}

static std::set<llvm::StringRef>
conjuctInstructionsFromExtensions(const std::vector<std::string> &Extensions) {
  std::set<llvm::StringRef> Conjuction = AllRISCVInstructions;
  for (auto &&Extension : Extensions) {
    std::set<llvm::StringRef> ExtensionInstrs =
        getInstructionsFromExtension(Extension);
    std::set<llvm::StringRef> FilteredInstrs;
    std::set_intersection(
        Conjuction.begin(), Conjuction.end(), ExtensionInstrs.begin(),
        ExtensionInstrs.end(),
        std::inserter(FilteredInstrs, FilteredInstrs.begin()));
    Conjuction = std::move(FilteredInstrs);
  }
  return Conjuction;
}

static std::set<llvm::StringRef>
filterByExtensions(const std::vector<RISCVExtensionCLEntry> &Extensions) {
  std::set<llvm::StringRef> SuitableInstrs;

  for (auto &&ExtensionEntry : Extensions) {
    std::set<llvm::StringRef> ExtensionInstrs =
        conjuctInstructionsFromExtensions(ExtensionEntry.Extensions);

    switch (ExtensionEntry.Operator) {
    case ExtensionOperator::DISJUNCTION: {
      SuitableInstrs.insert(ExtensionInstrs.begin(), ExtensionInstrs.end());
      break;
    }
    case ExtensionOperator::NEGATION: {
      std::set<llvm::StringRef> FilteredInstrs;
      std::set_difference(
          SuitableInstrs.begin(), SuitableInstrs.end(), ExtensionInstrs.begin(),
          ExtensionInstrs.end(),
          std::inserter(FilteredInstrs, FilteredInstrs.begin()));
      SuitableInstrs = std::move(FilteredInstrs);
      break;
    }
    };
  }

  return SuitableInstrs;
}

void RISCVInstructionEnumerator::initialize() {
  plugins::registerPlugin<InstructionEnumerator>(
      RISCVInstructionEnumerator::getPluginID(),
      RISCVInstructionEnumerator::getPluginCreateCallback());
}

void RISCVInstructionEnumerator::terminate() {
  plugins::unregisterPlugin<InstructionEnumerator>(
      RISCVInstructionEnumerator::getPluginID());
}

std::set<llvm::StringRef>
RISCVInstructionEnumerator::enumerateInstructions() const {
  std::set<llvm::StringRef> SuitableInstrs = filterByExtensions(ExtensionOpt);
  SuitableInstrs = filterByFeatures(SuitableInstrs, FeatureTable);
  return SuitableInstrs;
}
