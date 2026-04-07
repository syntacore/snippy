//===-- HistogramPatterns.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include "snippy/Config/ConfigIOContext.h"
#include "snippy/Config/HistogramPatterns.h"
#include "snippy/GeneratorUtils/LLVMState.h"
#include "snippy/Support/OpcodeCache.h"

#include <cctype>
#include <type_traits>

namespace llvm {
namespace snippy {
namespace {

struct HistogramExpressionParser final {
  enum Operations : char { Or = '|', Mul = '*', Pow = '^' };

  using PosIterator = StringRef::iterator;
  using ParserReturnType = Expected<BaseNode::NodeHandle>;

  HistogramExpressionParser(StringRef InputData, StringRef Name,
                            const OpcodeCache &OpCC,
                            const SnippyTarget &SnippyTgt,
                            HistogramPatterns &HistPatterns)
      : HistData(InputData), HistName(Name), Pos(HistData.begin()), OpCC(OpCC),
        SnippyTgt(SnippyTgt), HistPatterns(HistPatterns) {}

  ParserReturnType evaluateExpression() {
    auto Exp = parseExp();
    if (!Exp)
      return Exp;
    // Ensure that all data is parsed correctly until the end
    if (Pos != HistData.end())
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("unexpected data: '{0}'",
                        std::string(Pos, HistData.end())));
    return Exp;
  }

private:
  ParserReturnType parseExp() {
    auto LhsNode = parseTerm();
    if (!LhsNode)
      return LhsNode.takeError();
    skipWhitespaces();
    if (!isGivenOperation(Operations::Or))
      return LhsNode;
    auto ResultNode = std::make_unique<ChoiceNode>(std::move(*LhsNode));
    while (isGivenOperation(Operations::Or)) {
      Pos++;
      auto RhsNode = parseTerm();
      if (!RhsNode)
        return RhsNode.takeError();
      auto *ChoiceNodePtr = dyn_cast<ChoiceNode>(ResultNode.get());
      assert(ChoiceNodePtr);
      ChoiceNodePtr->insert(std::move(*RhsNode));
      skipWhitespaces();
    }
    return ResultNode;
  }

  ParserReturnType parseTerm() {
    skipWhitespaces();
    auto LhsNode = parsePower();
    if (!LhsNode)
      return LhsNode.takeError();
    skipWhitespaces();
    if (!isGivenOperation(Operations::Mul))
      return std::move(*LhsNode);
    auto ResultNode = std::make_unique<CartesianNode>(std::move(*LhsNode));
    while (isGivenOperation(Operations::Mul)) {
      Pos++;
      auto RhsNode = parsePower();
      if (!RhsNode)
        return RhsNode.takeError();
      auto *CartesianNodePtr = dyn_cast<CartesianNode>(ResultNode.get());
      assert(CartesianNodePtr);
      CartesianNodePtr->insert(std::move(*RhsNode));
    }
    return ResultNode;
  }

  ParserReturnType parsePower() {
    skipWhitespaces();
    auto LhsNode = parseFactor();
    if (!LhsNode)
      return LhsNode.takeError();
    skipWhitespaces();
    if (Pos != HistData.end() && *Pos == Operations::Pow) {
      Pos++;
      skipWhitespaces();
      // Try parsing something like that: pattern ^ [min : max]
      if (*Pos == '[') {
        Pos++;
        return parseRangeForRepeatNode(std::move(LhsNode));
      }
      auto ArgPtr = parseDegreeArgumentForRepeatNode();
      if (!ArgPtr)
        return ArgPtr;
      auto *DegreePtr = dyn_cast<NumberNode>(ArgPtr->get());
      assert(DegreePtr);
      return std::make_unique<RepeatNode>(std::move(*LhsNode),
                                          DegreePtr->getNum());
    }
    assert(LhsNode);
    return LhsNode;
  }

  ParserReturnType parseFactor() {
    skipWhitespaces();
    if (*Pos == '(') {
      Pos++;
      auto LhsNode = parseExp();
      if (!LhsNode)
        return LhsNode.takeError();
      assert(LhsNode);
      if (*Pos != ')')
        return createStringError(
            std::make_error_code(std::errc::invalid_argument),
            llvm::formatv("expected ')' in '{0}' initialization, got '{1}'",
                          HistName, *Pos));
      Pos++;
      return LhsNode;
    }
    return parseVariableName();
  }

  ParserReturnType parseVariableName() {
    auto Name = parseCharSequence();
    if (auto Tmp = 0; !StringRef(Name).getAsInteger(/* Radix */ 0, Tmp))
      return std::make_unique<NumberNode>(Name);
    if (auto Tmp = 0.0; !StringRef(Name).getAsDouble(Tmp))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          "floating-point values are not supported in this context");
    auto OpcodeOpt = OpCC.code(Name);
    if (OpcodeOpt.has_value()) {
      auto Opcode = OpcodeOpt.value();
      if (auto IsAllowed = isOpcodeAllowed(Opcode); !IsAllowed)
        return IsAllowed.takeError();
      auto OpcWeight = parseWeight();
      if (!OpcWeight)
        return OpcWeight.takeError();
      return std::make_unique<OpcodeNode>(Opcode, *OpcWeight);
    }
    if (!HistPatterns.contains(Name))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv(
              "unknown name or illegal opcode for specified cpu in '{0}' "
              "initialization: '{1}'.\n"
              "Use -list-opcode-names option to check for available "
              "instructions!",
              HistName, Name));
    auto HistWeight = parseWeight();
    if (!HistWeight)
      return HistWeight.takeError();
    return std::make_unique<HistogramNode>(Name, HistPatterns.clone(Name),
                                           *HistWeight);
  }

  ParserReturnType parseRangeForRepeatNode(ParserReturnType LhsNode) {
    if (!LhsNode)
      return LhsNode.takeError();
    auto LeftRange = parseDegreeArgumentForRepeatNode();
    if (!LeftRange)
      return LeftRange;
    auto *LeftRangeNum = dyn_cast<NumberNode>(LeftRange->get());
    assert(LeftRangeNum);
    skipWhitespaces();
    if (*Pos != ':')
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("expected ':' in '{0}' initialization", HistName));
    Pos++;
    skipWhitespaces();
    auto RightRange = parseDegreeArgumentForRepeatNode();
    if (!RightRange)
      return RightRange;
    auto *RightRangeNum = dyn_cast<NumberNode>(RightRange->get());
    if (*Pos != ']')
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("expected ']' in '{0}' initialization", HistName));
    Pos++;
    auto Min = LeftRangeNum->getNum();
    auto Max = RightRangeNum->getNum();
    if (Min > Max)
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv(
              "invalid range for '{0}' operation. Expected format: '[min : "
              "max]', where min <= max. Provided: '[{1} : {2}]'",
              Operations::Pow, Min, Max));
    return std::make_unique<RepeatNode>(std::move(*LhsNode),
                                        std::make_pair(Min, Max));
  }

  ParserReturnType parseDegreeArgumentForRepeatNode() {
    auto Arg = parseFactor();
    if (!Arg)
      return Arg.takeError();
    auto *ArgNum = dyn_cast<NumberNode>(Arg->get());
    if (!ArgNum)
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("invalid argument for '{0}' operation",
                        Operations::Pow));
    if (ArgNum->isNonPositive())
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("invalid argument for '{0}' operation. An integer "
                        "greater than 0 is expected",
                        Operations::Pow));
    return Arg;
  }

  // In the config, we can specify a weight for an opcode/pattern (optional).
  // E.g:
  // histogram-patterns:
  //   - PatternName: "... | PATTERN [WEIGHT1] | OPC [WEIGHT2] | ..."
  Expected<double> parseWeight() {
    skipWhitespaces();
    if (Pos == HistData.end() || *Pos != '[')
      return BaseNode::DefaultNodeWeight;
    // Skip '['
    ++Pos;
    auto WeightStr = parseCharSequence();
    double Result = 0.0;
    if (StringRef(WeightStr).getAsDouble(Result))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          llvm::formatv("expected floating-point value, got '{0}'", WeightStr));
    if (Pos == HistData.end() || *Pos != ']')
      return createStringError(
          std::make_error_code(std::errc::invalid_argument), "expected ']'");
    // Skip ']'
    ++Pos;
    return Result;
  }

  StringRef parseCharSequence() {
    skipWhitespaces();
    auto *Start = Pos;
    while (isAsciiAlpha(*Pos) || isAsciiDigit(*Pos) || *Pos == '_' ||
           *Pos == '-' || *Pos == '.')
      Pos++;

    StringRef Name(Start, Pos - Start);
    skipWhitespaces();
    return Name;
  }

  bool isGivenOperation(Operations Oper) const {
    return Pos != HistData.end() && *Pos == Oper;
  }

  void skipWhitespaces() {
    while (Pos != HistData.end() && std::isspace(*Pos))
      Pos++;
  }

  Expected<bool> isOpcodeAllowed(unsigned Opcode) const {
    const auto *Desc = OpCC.desc(Opcode);
    assert(Desc);
    if (Desc->isIndirectBranch() || Desc->isUnconditionalBranch() ||
        Desc->isBranch() || SnippyTgt.isCall(Desc->getOpcode()))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          "control-flow instructions are not supported");
    if (SnippyTgt.isVectorInstr(*Desc))
      return createStringError(
          std::make_error_code(std::errc::invalid_argument),
          "vector instructions are not supported");
    return true;
  }

public:
  StringRef HistData;
  StringRef HistName;
  PosIterator Pos;
  const OpcodeCache &OpCC;
  const SnippyTarget &SnippyTgt;
  HistogramPatterns &HistPatterns;
};

} // namespace
} // namespace snippy

using CustomTraits = yaml::CustomMappingTraits<snippy::HistogramPatternsEntry>;

void CustomTraits::inputOne(IO &IO, StringRef HistName,
                            snippy::HistogramPatternsEntry &Entry) {
  void *Ctx = IO.getContext();
  auto *HistPatternsCtx = static_cast<snippy::HistogramPatternsContext *>(Ctx);
  auto &InputCounts = HistPatternsCtx->InputCounts;
  InputCounts++;
  if (InputCounts > 1) {
    IO.setError("expected only one pattern definition");
    return;
  }
  IO.mapRequired(HistName.data(), Entry.Pattern);
  Entry.HistName = HistName;
  const auto &OpCC = HistPatternsCtx->OpCC;
  auto &HistPatterns = HistPatternsCtx->HistPatterns;
  if (HistPatterns.contains(HistName)) {
    IO.setError(llvm::formatv("redefinition of '{0}'", HistName));
    return;
  }
  snippy::HistogramExpressionParser HistParser(
      Entry.Pattern, HistName, OpCC, HistPatternsCtx->State.getSnippyTarget(),
      HistPatterns);
  auto HistRootNode = HistParser.evaluateExpression();
  if (!HistRootNode) {
    IO.setError(llvm::toString(HistRootNode.takeError()));
    return;
  }
  HistPatterns.appendHistogram(HistName, std::move(*HistRootNode));
}

void CustomTraits::output(IO &IO, snippy::HistogramPatternsEntry &Entry) {
  IO.mapRequired(Entry.HistName.data(), Entry.Pattern);
}

size_t yaml::SequenceTraits<snippy::HistogramPatterns>::size(
    IO &IO, snippy::HistogramPatterns &Seq) {
  return Seq.NameToPattern.size();
}

snippy::HistogramPatternsEntry &
yaml::SequenceTraits<snippy::HistogramPatterns>::element(
    IO &IO, snippy::HistogramPatterns &Seq, size_t Index) {
  // Needs to update the InputCounts counter to keep track of the number of
  // elements in each sequence within the CustomTraits.
  void *Ctx = IO.getContext();
  auto *HistPatternsCtx = static_cast<snippy::HistogramPatternsContext *>(Ctx);
  HistPatternsCtx->InputCounts = 0;
  if (Index >= Seq.NameToPattern.size())
    Seq.NameToPattern.resize(Index + 1);
  return Seq.NameToPattern[Index];
}

} // namespace llvm
