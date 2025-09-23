//===--- FeatureFilter.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FeatureFilter.h"
#include "CommandLineOpts.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/WithColor.h"

#include <vector>

using namespace llvm;

static std::vector<std::string> getEnabledFeatureOptions() {
  StringMap<cl::Option *> &Options = cl::getRegisteredOptions();

  using MapEntryRef =
      std::reference_wrapper<StringMap<cl::Option *>::MapEntryTy>;
  std::vector<MapEntryRef> EnabledFeatureOpts{};

  llvm::copy_if(Options, std::back_inserter(EnabledFeatureOpts),
                [](auto &&Entry) {
                  cl::Option *Option = Entry.getValue();
                  if (!llvm::is_contained(Option->Categories,
                                          &opts::FeatureOptionsCategory))
                    return false;
                  cl::opt<bool> *Opt = static_cast<cl::opt<bool> *>(Option);
                  return Opt->getValue();
                });

  std::vector<std::string> OptNames{};
  llvm::transform(EnabledFeatureOpts, std::back_inserter(OptNames),
                  [](auto &&Entry) {
                    cl::Option *Option = Entry.get().getValue();
                    return Option->ArgStr.str();
                  });
  return OptNames;
}

static std::set<llvm::StringRef>
applyFeature(const std::set<llvm::StringRef> &Instrs, llvm::StringRef Feature,
             const llvm::DenseMap<llvm::StringRef, std::set<llvm::StringRef>>
                 &FeatureTable) {
  auto it = FeatureTable.find(Feature);
  if (it == FeatureTable.end()) {
    llvm::WithColor::warning()
        << "\'--" << Feature
        << "\' is unsupported on this target! Skipping this option.\n";
    return Instrs;
  }

  const std::set<llvm::StringRef> &Filter = it->second;
  std::set<llvm::StringRef> FilteredInstrs{};
  std::set_intersection(Instrs.begin(), Instrs.end(), Filter.begin(),
                        Filter.end(),
                        std::inserter(FilteredInstrs, FilteredInstrs.begin()));
  return FilteredInstrs;
}

namespace llvm_ie {

std::set<llvm::StringRef> filterByFeatures(
    const std::set<llvm::StringRef> &Instrs,
    const llvm::DenseMap<llvm::StringRef, std::set<llvm::StringRef>>
        &FeatureTable) {
  std::set<llvm::StringRef> SuitableInstrs(Instrs.begin(), Instrs.end());
  for (auto &&Feature : getEnabledFeatureOptions())
    SuitableInstrs = applyFeature(SuitableInstrs, Feature, FeatureTable);
  return SuitableInstrs;
}

} // namespace llvm_ie
