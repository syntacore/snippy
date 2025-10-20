//===--- RISCVExtensionCLParser.h -------- Command Line Options -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_LIB_RISCV_RISCV_EXTENSION_CL_PARSER_H
#define LLVM_TOOLS_LLVM_LIB_RISCV_RISCV_EXTENSION_CL_PARSER_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"

#include <regex>
#include <vector>

namespace llvm_ie {

enum class ExtensionOperator { DISJUNCTION, NEGATION };

struct RISCVExtensionCLEntry {
  RISCVExtensionCLEntry(const std::vector<std::string> &ExtensionNames,
                        ExtensionOperator ExtOp)
      : Extensions{ExtensionNames}, Operator{ExtOp} {}

  std::vector<std::string> Extensions;
  ExtensionOperator Operator;
};

class RISCVExtensionParser
    : public llvm::cl::parser<std::vector<RISCVExtensionCLEntry>> {
public:
  RISCVExtensionParser(llvm::cl::Option &O)
      : llvm::cl::parser<std::vector<RISCVExtensionCLEntry>>(O) {}

  bool parse(llvm::cl::Option &O, llvm::StringRef ArgName,
             llvm::StringRef ArgValue,
             std::vector<RISCVExtensionCLEntry> &Vals) {
    auto TokensOrErr = GetTokensFromsArgument(ArgValue.str());
    if (llvm::Error E = TokensOrErr.takeError())
      return O.error(llvm::toString(std::move(E)));

    Vals = std::move(TokensOrErr.get());
    return false;
  }

private:
  /// Parses -riscv-ext command line option.
  /// param [in] ArgValue
  ///     Contains a string representing an extension expression. The expression
  ///     consists of RISC-V extension's names with logical operations between
  ///     them. Currently, only 3 operations are permited in the expressions:
  ///       * DISJUNCTION (+)
  ///       * CONJUNCTION (&)
  ///       * NEGATION (-)
  ///     & opetation has a higher priority than + and - ones, therefor the
  ///     expression 'a + d&c - zaamo' will conjunct D and C extensions and only
  ///     after that will perform other operations. Expression may contain 'all'
  ///     (case insensitive) keyword, which reflexes all RISC-V extensions.
  /// \return
  ///     Vector of the RISCVExtensionCLEntries. Each entry contains a
  ///     vector of the extensions to conjunct and the previous operator
  ///     (DISJUNCTION or NEGATION). For example, for the 'a + d&c - zaamo'
  ///     expression the resulting vector will consists of the following
  ///     entries:
  ///     {{a}, DISJUNCTION}, {{d, c}, DISJUNCTION}, {{zaamo}, NEGATION}
  static llvm::Expected<std::vector<RISCVExtensionCLEntry>>
  GetTokensFromsArgument(const std::string &ArgValue) {
    const std::regex R("\\s+");

    auto Begin =
        std::sregex_token_iterator(ArgValue.begin(), ArgValue.end(), R, -1);
    auto End = std::sregex_token_iterator();
    auto Tokens = llvm::iterator_range(Begin, End);

    if (Tokens.empty())
      return llvm::createStringError(
          llvm::formatv("Invalid extension list format: {0}", ArgValue));

    std::vector<RISCVExtensionCLEntry> ExtensionExtries;
    ExtensionOperator Operator = ExtensionOperator::DISJUNCTION;
    std::vector<std::string> Extensions;

    for (std::string TokenStr : Tokens) {
      llvm::StringRef Token = TokenStr;
      if (Token == "+") {
        ExtensionExtries.emplace_back(Extensions, Operator);
        Operator = ExtensionOperator::DISJUNCTION;
        Extensions.clear();
        continue;
      }

      if (Token == "-") {
        ExtensionExtries.emplace_back(Extensions, Operator);
        Operator = ExtensionOperator::NEGATION;
        Extensions.clear();
        continue;
      }

      while (Token.contains("&")) {
        auto [LhsExt, RhsExt] = Token.split("&");
        Extensions.push_back(LhsExt.lower());
        Token = RhsExt;
      }
      Extensions.push_back(Token.lower());
    }

    ExtensionExtries.emplace_back(Extensions, Operator);

    return ExtensionExtries;
  }
};

} // namespace llvm_ie
#endif
