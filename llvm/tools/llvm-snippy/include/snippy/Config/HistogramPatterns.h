//===-- HistogramPatterns.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_HISTOGRAMPATTERNS_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_HISTOGRAMPATTERNS_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Config/OpcodeHistogramNode.h"
#include "snippy/Support/YAMLUtils.h"

#include <memory>

namespace llvm {
namespace snippy {

class OpcodeHistogram;
class LLVMState;
class OpcodeCache;

struct HistogramPatternsEntry final {
  std::string HistName;
  std::string Pattern;
};

class HistogramPatterns final {
public:
  HistogramPatterns() = default;
  HistogramPatterns(const HistogramPatterns &Rhs) {
    for (auto &&[HistName, HistPtr] : Rhs.Histograms)
      Histograms.insert_or_assign(HistName, HistPtr->clone());
  };

  HistogramPatterns(HistogramPatterns &&Rhs) = default;
  HistogramPatterns &operator=(const HistogramPatterns &Rhs) {
    if (this == std::addressof(Rhs))
      return *this;

    auto Tmp = Rhs;
    NameToPattern = std::move(Tmp.NameToPattern);
    Histograms = std::move(Tmp.Histograms);
    return *this;
  }

  HistogramPatterns &operator=(HistogramPatterns &&Rhs) = default;
  ~HistogramPatterns() = default;

  void appendHistogram(StringRef HistName, BaseNode::NodeHandle InitNode) {
    assert(InitNode);
    Histograms.insert_or_assign(HistName, std::move(InitNode));
  }

  bool contains(StringRef HistName) const {
    return Histograms.contains(HistName);
  }

  const BaseNode &get(StringRef HistName) const { return getImpl(HistName); }

  BaseNode &get(StringRef HistName) { return getImpl(HistName); }

  BaseNode::NodeHandle clone(StringRef HistName) const {
    return get(HistName).clone();
  }

  unsigned size() const { return Histograms.size(); }

  // begin()/end() are used for SequenceTraits
  auto begin() { return NameToPattern.begin(); }
  auto end() { return NameToPattern.end(); }

  // Used only while mapping
  SmallVector<HistogramPatternsEntry> NameToPattern;
  // Histogram name to its defining node
  StringMap<BaseNode::NodeHandle> Histograms;

  friend class OpcodeHistogram;

private:
  BaseNode &getImpl(StringRef HistName) const {
    assert(contains(HistName) &&
           llvm::formatv("'{0}' was not defined", HistName).str().c_str());
    auto Found = Histograms.find(HistName);
    assert(Found != Histograms.end());
    return *Found->second.get();
  }
};

// It needs for passing to yaml::IO context while mapping
struct HistogramPatternsContext final {
  HistogramPatternsContext(ConfigIOContext &Ctx,
                           HistogramPatterns &HistPatterns)
      : OpCC(Ctx.OpCC), State(Ctx.State), HistPatterns(HistPatterns) {}

  HistogramPatternsContext(const OpcodeCache &OpCC, LLVMState &State,
                           HistogramPatterns &HistPatterns)
      : OpCC(OpCC), State(State), HistPatterns(HistPatterns) {}

  const OpcodeCache &OpCC;
  LLVMState &State;
  HistogramPatterns &HistPatterns;
  // To check how many CustomMappingTraits elements were read
  unsigned InputCounts;
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_CUSTOM_MAPPING_TRAITS(snippy::HistogramPatternsEntry);
// Using SequenceTraits for the mapping (rather than just CustomMappingTraits)
// to maintain a clear order of defined patterns, which allows us to use a
// previous pattern when initializing a new one.
LLVM_SNIPPY_YAML_DECLARE_SEQUENCE_TRAITS(snippy::HistogramPatterns,
                                         snippy::HistogramPatternsEntry);
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_HISTOGRAMPATTERNS_H
