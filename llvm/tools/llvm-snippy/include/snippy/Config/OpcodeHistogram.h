//===-- OpcodeHistogram.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/HistogramPatterns.h"
#include "snippy/Config/OpcodeHistogramVisitor.h"
#include "snippy/Support/OpcodeCache.h"
#include "snippy/Support/YAMLHistogram.h"
#include "snippy/Support/YAMLUtils.h"

#include "llvm/MC/MCInstrDesc.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>
#include <string>
#include <type_traits>

namespace llvm {
struct OpcodeHistogramNormalization;
namespace snippy {

struct OpcodeHistogramSequenceValue final {
  using SeqValueType = std::string;
  OpcodeHistogramSequenceValue() = default;
  OpcodeHistogramSequenceValue(const SeqValueType &Val, yaml::NodeKind Kind)
      : Val(Val), Kind(Kind) {}

  SeqValueType Val;
  yaml::NodeKind Kind;
};

struct OpcodeHistogramSequence final {
  // Sequence of opcode/pattern and weight
  using SequenceType = SmallVector<OpcodeHistogramSequenceValue, 2>;
  using SeqValueType = OpcodeHistogramSequenceValue::SeqValueType;
  // Set of opcode/pattern and weight pairs
  using EntryWeightSeqType =
      SmallVector<std::pair<OpcodeHistogramSequenceValue, SeqValueType>>;

  auto begin() { return Data.begin(); }
  auto end() { return Data.end(); }

  EntryWeightSeqType getEntryWeightSequence() const {
    EntryWeightSeqType Result;
    Result.reserve(Data.size());
    llvm::transform(Data, std::back_inserter(Result), [](auto &&Sequence) {
      assert(Sequence.size() == 2);
      return std::make_pair(Sequence.front(), Sequence.back().Val);
    });
    return Result;
  }

  void validate(yaml::IO &IO) const;

  SmallVector<SequenceType> Data;
};

class SnippyTarget;
struct ConfigIOContext;

struct OpcodeHistogramEntry {
  // If Weight set to this value (in fact any negative one) this
  // means that user should ignore the respected entry
  static constexpr double IgnoredWeight = -1.0;
  bool deactivated() const { return Weight < 0.0; }

  unsigned Opcode;
  double Weight;
};

// OpcodeHistogram is a tree structure that stores opcodes and complete patterns
// along with the probabilities with which they can be generated. Opcodes
// specified directly in the configuration are top-level opcodes. In turn,
// patterns can either be simply individually specified opcodes (in the simplest
// case) or entire sequences of opcodes
// An example visualization of the OpcodeHistogram (Opc: top-level opcode,
// W: weight)
//                             OpcodeHistogram
//   --------------------------------------------------------------------------
//    |                |                |             |               |
//   [Opc1, W1]   [Pattern1, W2]    [Opc2, W3]    [Opc3, W4]     [Pattern2, W5]
//
class OpcodeHistogram final : private ChoiceNode {
  using ChoiceNode::evaluate;
  using ChoiceNode::getTotalChildsWeight;
  using ChoiceNode::insert;

  auto getTopNodesPtrRange() const {
    return llvm::map_range(ChildNodes, [](auto &&PtrNode) {
      assert(PtrNode);
      return std::make_pair(PtrNode.get(), PtrNode->getWeight());
    });
  }

public:
  using ChoiceNode::empty;
  using ChoiceNode::size;
  using TopOpcodesType = std::map<unsigned, double>;

  OpcodeHistogram() = default;
  template <typename OpcWeightRange>
  OpcodeHistogram(OpcWeightRange &&TopOpcodes) {
    insertTopOpcodes(TopOpcodes);
  }

  void generate(SmallVectorImpl<unsigned> &Opcodes) const {
    llvm::append_range(Opcodes, evaluate());
  }

  auto patterns() const {
    return llvm::make_filter_range(
        getTopNodesPtrRange(), [this](auto &&TopNodeWeight) {
          return !isTopOpcodePtr(TopNodeWeight.first);
        });
  }

  void copyPatterns(const OpcodeHistogram &Rhs) {
    for (auto &&[Pattern, _] : Rhs.patterns()) {
      assert(Pattern);
      insert(Pattern->clone());
    }
    reinitOpcodesState();
  }
  // Inserts a range of opcode-weight pairs.
  template <typename ContainterTy> void insertTopOpcodes(ContainterTy &&Cont) {
    llvm::for_each(Cont, [&](auto &&OpcWeightPair) {
      auto &&[Opc, Weight] = OpcWeightPair;
      insertTopOpcode(Opc, Weight);
    });
    reinitOpcodesState();
  }

  template <typename IterTy> void insertTopOpcodes(IterTy Begin, IterTy End) {
    insertTopOpcodes(llvm::make_range(Begin, End));
  }

  // get top-level opcode-weight pairs
  const TopOpcodesType &topOpcodes() const { return TopOpcodes; }
  TopOpcodesType &topOpcodes() { return TopOpcodes; }
  // get all unique opcodes including top-level histogram and all patterns
  auto uniqueOpcodes() const {
    return llvm::make_first_range(ProbVisitor.opcodeProbabilities());
  }

  template <typename Predicate> void eraseTopOpcodes(Predicate &&Pred) {
    llvm::erase_if(ChildNodes, [&Pred, this](auto &&ArgPtr) {
      auto *NodePtr = ArgPtr.get();
      assert(NodePtr);
      if (isTopOpcodePtr(NodePtr))
        return Pred(dyn_cast<OpcodeNode>(NodePtr)->getNum());
      return false;
    });
    // FIXME: Replace with std::erase_if when there is C++20
    // Erase from cache
    for (auto It = TopOpcodes.begin(); It != TopOpcodes.end();) {
      if (Pred(It->first))
        It = TopOpcodes.erase(It);
      else
        ++It;
    }
    reinitOpcodesState();
  }

  // Aggregated weight of all opcodes, considering both standalone and
  // pattern-embedded usage
  double getTotalWeight() const { return ProbVisitor.getTotalWeight(); }

  // Probability of an opcode appearing either as a standalone instruction or
  // within a pattern
  double probability(unsigned Opcode) const {
    return ProbVisitor.getProbability(Opcode);
  }

  // Weight of an opcode appearing either as a standalone instruction or
  // within a pattern
  double weight(unsigned Opcode) const {
    return probability(Opcode) * getTotalWeight();
  }

  double getTopOpcodesWeight(std::function<bool(unsigned)> Pred =
                                 [](unsigned Opc) { return true; }) const;

  double getOpcodesProbability(std::function<bool(unsigned)> Pred) const {
    auto OpcProbs = opcodeProbabilities();
    return std::accumulate(OpcProbs.begin(), OpcProbs.end(), 0.0,
                           [&Pred](double Accumulation, auto &&Hist) -> double {
                             if (Pred(Hist.first))
                               return Accumulation + Hist.second;
                             return Accumulation;
                           });
  }

  double getCFProbability(const OpcodeCache &OpCC) const {
    return getOpcodesProbability([&OpCC](unsigned Opcode) {
      auto *Desc = OpCC.desc(Opcode);
      return Desc && Desc->isBranch();
    });
  }

  bool hasCFInstrs(const OpcodeCache &OpCC) const {
    // CF instructions can only be present in the top-level histogram.
    return llvm::any_of(TopOpcodes, [&OpCC](auto &Hist) {
      auto *Desc = OpCC.desc(Hist.first);
      return Desc && Desc->isBranch();
    });
  }

  bool hasUncondBranches(const OpcodeCache &OpCC) const {
    // Branche instructions can only be present in the top-level histogram.
    return llvm::any_of(TopOpcodes, [&OpCC](auto &Hist) {
      auto *Desc = OpCC.desc(Hist.first);
      return Desc && Desc->isUnconditionalBranch();
    });
  }

  bool hasIndirectBranches(const OpcodeCache &OpCC) const {
    // Branche instructions can only be present in the top-level histogram.
    return llvm::any_of(TopOpcodes, [&OpCC](auto &Hist) {
      auto *Desc = OpCC.desc(Hist.first);
      return Desc && Desc->isIndirectBranch();
    });
  }

  const OpcodeProbVisitor::OpcodeProbsType &opcodeProbabilities() const;

  bool hasCallInstrs(const OpcodeCache &OpCC, const SnippyTarget &Tgt) const;
  unsigned getCFInstrsNum(unsigned InstrsNum, const OpcodeCache &OpCC) const;

  bool hasSPRelativeInstrs(const OpcodeCache &OpCC,
                           const SnippyTarget &Tgt) const;
  bool hasPlainInstrs(const OpcodeCache &OpCC, const SnippyTarget &Tgt) const;

  bool contains(unsigned OpcodeToFind) const;

  OpcodeProbVisitor::OpcProbOpt find(unsigned OpcodeToFind) const;

  bool isProbabilityZero(unsigned Opcode) const {
    return probability(Opcode) < std::numeric_limits<double>::epsilon();
  }

  template <typename... OpcodeT> bool isAnyNonZero(OpcodeT... Opcodes) const {
    static_assert((std::is_convertible_v<OpcodeT, unsigned> && ...) &&
                  "Opcode types must be convertible to unsigned");
    return (!isProbabilityZero(Opcodes) || ...);
  }

  bool hasPatterns() const { return HasPatterns; }

  friend struct llvm::OpcodeHistogramNormalization;

private:
  // After any modifications to the histogram (insertions / deletions), we must
  // recalculate the weights (probabilities) for all opcodes.
  void reinitOpcodesState() {
    reinitProbabilityVisitor();
    reinitTopOpcodeWeights();
  }

  void reinitProbabilityVisitor() {
    ProbVisitor.reinit(static_cast<ChoiceNode>(*this));
  }

  void reinitTopOpcodeWeights() {
    for (auto &&[Opc, Weight] : TopOpcodes)
      Weight = weight(Opc);
  }

  void insertTopOpcode(unsigned Opc, double Weight) {
    emplace<OpcodeNode>(Opc, Weight, OpcodeNode::OpcodeCategory::Top);
    TopOpcodes.emplace(Opc, Weight);
  }

  bool isTopOpcodePtr(BaseNode *NodePtr) const {
    auto *OpcPtr = dyn_cast<OpcodeNode>(NodePtr);
    return OpcPtr && OpcPtr->isTopOpcode();
  }

  bool HasPatterns = false;
  // To prevent constantly traversing the tree in search of top opcodes,
  // we store them as an additional field.
  TopOpcodesType TopOpcodes;
  OpcodeProbVisitor ProbVisitor;
};

struct OpcodeHistogramDecodedEntry {
  OpcodeHistogramDecodedEntry(StringRef RP = "") : RegexPattern(RP) {}
  OpcodeHistogramDecodedEntry(std::initializer_list<OpcodeHistogramEntry> List)
      : Decoded(List.begin(), List.end()) {}

  // Each histogram entry can correspond to multiple opcode -> weight mappings
  SmallVector<OpcodeHistogramEntry> Decoded;
  std::string RegexPattern;
};

struct OpcodeHistogramCodedEntry {
  std::string InstrMnemonic;
  std::string Weight;
};

struct OpcodeHistogramMappingWrapper final {
  OpcodeHistogram &Histogram;
};

} // namespace snippy

using SequenceType = snippy::OpcodeHistogramSequence::SequenceType;
template <> struct yaml::SequenceTraits<snippy::OpcodeHistogramSequence> {

  static size_t size(IO &IO, snippy::OpcodeHistogramSequence &Seq);

  static SequenceType &element(IO &IO, snippy::OpcodeHistogramSequence &Seq,
                               size_t Index);
};

template <> struct yaml::SequenceTraits<SequenceType> {
  static size_t size(IO &IO, SequenceType &Seq) { return Seq.size(); }

  static snippy::OpcodeHistogramSequenceValue &
  element(IO &IO, SequenceType &Seq, size_t Index) {
    if (Index > 1)
      IO.setError("Incorrect histogram: Only two parameters (key and weight) "
                  "are allowed in a histogram");
    if (Index >= Seq.size())
      Seq.resize(Index + 1);
    return Seq[Index];
  }
};

// For pattern name mapping
struct OpcodeHistogramMapValue final {
  std::string &Val;
};

template <> struct yaml::MappingTraits<const OpcodeHistogramMapValue> {
  static void mapping(IO &IO, const OpcodeHistogramMapValue &Val) {
    IO.mapOptional("pattern", const_cast<std::string &>(Val.Val));
  }

  static std::string validate(IO &IO, const OpcodeHistogramMapValue &Val) {
    return "";
  }
};

template <>
struct yaml::PolymorphicTraits<snippy::OpcodeHistogramSequenceValue> {
  static yaml::NodeKind
  getKind(const snippy::OpcodeHistogramSequenceValue &Info) {
    return Info.Kind;
  }

  static std::string &getAsScalar(snippy::OpcodeHistogramSequenceValue &Info) {
    Info.Kind = NodeKind::Scalar;
    return Info.Val;
  }

  // FIXME: Add better diagnostics here
  static std::string &
  getAsSequence(snippy::OpcodeHistogramSequenceValue &Info) {
    llvm_unreachable("sequence is not allowed in histogram initialization");
  }

  static const OpcodeHistogramMapValue
  getAsMap(snippy::OpcodeHistogramSequenceValue &Info) {
    Info.Kind = NodeKind::Map;
    return {Info.Val};
  }
};

struct OpcodeHistogramNormalization final {
  snippy::OpcodeHistogramSequence OpcHistSeq;
  snippy::HistogramPatterns HistPatterns;
  std::string DefineMainHist;

  OpcodeHistogramNormalization(yaml::IO &IO);
  OpcodeHistogramNormalization(yaml::IO &IO,
                               const snippy::OpcodeHistogram &HistData);

  snippy::OpcodeHistogram denormalize(yaml::IO &IO);

  void mapHistogramPatterns(yaml::IO &IO);

  // Returns non empty error string if something went wrong
  std::string insertHistogramNode(snippy::OpcodeHistogram &Hist, StringRef Name,
                                  double Weight);

private:
  // Returns true if 'WeightStr' contains a non-negative floating-point value
  // and isn't NaN or inf. If the return value is true, 'Weight' will be set to
  // the double representation of 'WeightStr'. Otherwise (if the return value is
  // false), 'Weight' will remain unchanged.
  static bool verifyAndSetWeight(StringRef WeightStr, double &Weight) {
    return not(WeightStr.getAsDouble(Weight, true) or std::isnan(Weight) or
               std::isinf(Weight) or Weight < 0.0);
  }
};

snippy::OpcodeHistogramCodedEntry
codeInstrFromOpcode(yaml::IO &IO, const snippy::OpcodeHistogramDecodedEntry &E);

Expected<snippy::OpcodeHistogramDecodedEntry>
decodeInstrRegex(yaml::IO &IO, StringRef OpcodeStr, double Weight);

LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::OpcodeHistogramMappingWrapper);
namespace yaml {
void yamlize(yaml::IO &, snippy::OpcodeHistogramDecodedEntry &, bool,
             EmptyContext &Ctx);
} // namespace yaml
} // namespace llvm
