//===-- DiagnostricInfo.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/Options.h"

#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/WithColor.h"

#include <unordered_set>

namespace llvm {

// The unique option because it used not only in snippy
cl::OptionCategory DiagnosticOptions("llvm-diagnostic");

namespace snippy {

static snippy::opt_list<std::string>
    WError("Werror", cl::CommaSeparated,
           cl::desc("Comma separated list of warnig types that should be "
                    "treated as errors"),
           cl::cat(llvm::DiagnosticOptions), cl::ValueOptional);

static snippy::opt_list<std::string>
    WNoError("Wno-error", cl::CommaSeparated,
             cl::desc("Comma separated list of warning types that should be "
                      "excluded from WError option"),
             cl::cat(llvm::DiagnosticOptions), cl::ValueOptional);

static snippy::opt_list<std::string>
    WDisable("Wdisable", cl::CommaSeparated,
             cl::desc("Comma-separated list of warning types to suppress"),
             cl::cat(DiagnosticOptions));

#ifdef WARN_CASE
#error WARN_CASE should not be defined at this point
#else
#define WARN_CASE(NAME, STR)                                                   \
  case WarningName::NAME:                                                      \
    return WarningNameOf<WarningName::NAME>;
#endif
StringLiteral getWarningNameStr(WarningName Warn) {
  switch (Warn) { FOR_ALL_WARNINGS(WARN_CASE) }
  llvm_unreachable("Unsupported WarningName value");
}
#undef WARN_CASE

#ifdef WARN_CASE
#error WARN_CASE should not be defined at this point
#else
#define WARN_CASE(NAME, STR)                                                   \
  .Case(WarningNameOf<WarningName::NAME>, WarningName::NAME)
#endif
std::optional<WarningName> getWarningName(StringRef Warn) {
  // clang-format off
  return StringSwitch<std::optional<WarningName>>(Warn)
    FOR_ALL_WARNINGS(WARN_CASE)
    .Default(std::nullopt);
  // clang-format on
}
#undef WARN_CASE

const int SnippyDiagnosticInfo::KindID = getNextAvailablePluginDiagnosticKind();
std::map<std::string, WarningCounters> SnippyDiagnosticInfo::ReportedWarnings;

std::vector<std::pair<std::string, size_t>>
SnippyDiagnosticInfo::fetchReportedWarnings() {
  std::vector<std::pair<std::string, size_t>> Result;
  std::transform(ReportedWarnings.begin(), ReportedWarnings.end(),
                 std::back_inserter(Result), [](const auto &Item) {
                   return std::make_pair(Item.first,
                                         Item.second.EncounteredTotal);
                 });
  std::sort(Result.begin(), Result.end(), [](const auto &L, const auto &R) {
    auto LOrder = ReportedWarnings[L.first].EncounteredOrder;
    auto ROrder = ReportedWarnings[R.first].EncounteredOrder;
    return LOrder < ROrder;
  });
  return Result;
}

static void checkDiagnosticsTypesAreValid(const opt_list<std::string> &Types) {
  auto Unknown = make_filter_range(
      Types, [](StringRef Name) { return !getWarningName(Name).has_value(); });
  std::vector<StringRef> UnknownNames(Unknown.begin(), Unknown.end());
  if (!UnknownNames.empty()) {
    std::string Msg;
    raw_string_ostream OS(Msg);
    OS << "List of unknown warning categories: ";
    llvm::interleaveComma(UnknownNames, OS,
                          [&](StringRef N) { OS << "\"" << N << "\""; });
    snippy::fatal(Twine("Unknown warning category specified for \"")
                      .concat(Types.ArgStr)
                      .concat("\" option"),
                  Msg);
  }
}

class WErrorCategories final {
public:
  static auto &instance() {
    static std::unordered_set<WarningName> Impl = {
        WarningName::NonReproducibleExecution};
    return Impl;
  }
};

static bool isOptListEmpty(const opt_list<std::string> &List) {
  // FIXME: Option parser considers option list without arguments to have single
  // entity (which is empty string) I.e. -opt and -opt="" are the same for opt
  // list
  return (List.size() == 0) || (List.size() == 1 && *List.begin() == "");
}

#define WARN_CASE(NAME, STR) WErrorCat.insert(WarningName::NAME);
void checkWarningOptions() {
  checkDiagnosticsTypesAreValid(WDisable);
  auto &WErrorCat = WErrorCategories::instance();
  if (!isOptListEmpty(WError)) {
    checkDiagnosticsTypesAreValid(WError);
    transform(WError, std::inserter(WErrorCat, WErrorCat.end()),
              [](StringRef WN) {
                auto Category = getWarningName(WN);
                assert(Category);
                return *Category;
              });
  } else if (WError.getNumOccurrences()) {
    // Empty -Werror means all warning are considered errors
    FOR_ALL_WARNINGS(WARN_CASE)
  }

  if (!isOptListEmpty(WNoError)) {
    checkDiagnosticsTypesAreValid(WNoError);
    for (StringRef WN : WNoError)
      WErrorCat.erase(*getWarningName(WN));
  } else if (WNoError.getNumOccurrences()) {
    // Empty -Wno-error means none of the warnings should be considered error
    WErrorCat.clear();
  }
}
#undef WARN_CASE

void SnippyDiagnosticInfo::print(llvm::DiagnosticPrinter &DP) const {
  if (getName() != WarningName::NotAWarning) {
    DP << "(" << getWarningNameStr(getName()) << ") ";
  }
  DP << Description;

  if (getSeverity() == llvm::DS_Error)
    return;

  if (getSeverity() >= llvm::DS_Remark)
    return;

  auto WarningIt = ReportedWarnings.find(Description);
  if (WarningIt == ReportedWarnings.end())
    ReportedWarnings.insert(std::make_pair(
        Description,
        WarningCounters{/* EncounteredOrder */ ReportedWarnings.size(),
                        /* EncounteredTotal */ 1u}));
  else
    ++WarningIt->second.EncounteredTotal;
}

void handleDiagnostic(LLVMContext &Ctx, const SnippyDiagnosticInfo &Diag) {
  if (Diag.getSeverity() != DS_Error) {
    auto FoundIgnore = find(WDisable, getWarningNameStr(Diag.getName()));
    if (FoundIgnore != WDisable.end())
      return;
  }
  auto OldHandlerCallback = Ctx.getDiagnosticHandlerCallBack();

  Ctx.setDiagnosticHandlerCallBack([](const DiagnosticInfo *Info, void *) {
    HighlightColor Color = [&]() {
      switch (Info->getSeverity()) {
      case DS_Error:
        return HighlightColor::Error;
      case DS_Remark:
        return HighlightColor::Remark;
      case DS_Warning:
        return HighlightColor::Warning;
      case DS_Note:
        return HighlightColor::Note;
      }
    }();

    WithColor(errs(), Color)
        << LLVMContext::getDiagnosticMessagePrefix(Info->getSeverity());

    errs() << ": ";

    DiagnosticPrinterRawOStream DP(errs());
    Info->print(DP);
    errs() << "\n";
    if (Info->getSeverity() == DS_Error)
      exit(1);
  });

  Ctx.diagnose(Diag);
  Ctx.setDiagnosticHandlerCallBack(OldHandlerCallback);
}

void handleDiagnostic(const SnippyDiagnosticInfo &Diag) {
  LLVMContext Ctx;
  handleDiagnostic(Ctx, Diag);
}

void notice(const llvm::Twine &Prefix, const llvm::Twine &Desc,
            WarningName WN) {
  SnippyDiagnosticInfo Diag(Prefix, Desc, llvm::DS_Remark,
                            WarningName::NotAWarning);
  handleDiagnostic(Diag);
}

void notice(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
            const llvm::Twine &Desc, WarningName WN) {
  SnippyDiagnosticInfo Diag(Prefix, Desc, llvm::DS_Remark,
                            WarningName::NotAWarning);
  handleDiagnostic(Ctx, Diag);
}

void notice(WarningName WN, llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
            const llvm::Twine &Desc) {
  SnippyDiagnosticInfo Diag(Prefix, Desc, llvm::DS_Remark, WN);
  handleDiagnostic(Ctx, Diag);
}

void warn(WarningName WN, llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
          const llvm::Twine &Desc) {
  bool IsErr = is_contained(WErrorCategories::instance(), WN);
  auto MsgCategory = IsErr ? llvm::DS_Error : llvm::DS_Warning;
  SnippyDiagnosticInfo Diag(Prefix, Desc, MsgCategory, WN);
  handleDiagnostic(Ctx, Diag);
}

void warn(WarningName WN, const llvm::Twine &Prefix, const llvm::Twine &Desc) {
  bool IsErr = is_contained(WErrorCategories::instance(), WN);
  auto MsgCategory = IsErr ? llvm::DS_Error : llvm::DS_Warning;
  SnippyDiagnosticInfo Diag(Prefix, Desc, MsgCategory, WN);
  handleDiagnostic(Diag);
}

[[noreturn]] void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
                        const llvm::Twine &Desc) {
  SnippyDiagnosticInfo Diag(Prefix, Desc, llvm::DS_Error,
                            WarningName::NotAWarning);
  handleDiagnostic(Ctx, Diag);
  llvm_unreachable("snippy::fatal should never return");
}

[[noreturn]] void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
                        Error E) {
  SnippyDiagnosticInfo Diag(Prefix, toString(std::move(E)), llvm::DS_Error,
                            WarningName::NotAWarning);
  handleDiagnostic(Ctx, Diag);
  llvm_unreachable("snippy::fatal should never return");
}

[[noreturn]] void fatal(Error E) {
  SnippyDiagnosticInfo Diag(toString(std::move(E)), llvm::DS_Error,
                            WarningName::NotAWarning);
  handleDiagnostic(Diag);
  llvm_unreachable("snippy::fatal should never return");
}

[[noreturn]] void fatal(const llvm::Twine &Prefix, const llvm::Twine &Desc) {
  SnippyDiagnosticInfo Diag(Prefix, Desc, llvm::DS_Error,
                            WarningName::NotAWarning);
  handleDiagnostic(Diag);
  llvm_unreachable("snippy::fatal should never return");
}

[[noreturn]] void fatal(const llvm::Twine &Desc) {
  SnippyDiagnosticInfo Diag(Desc, llvm::DS_Error, WarningName::NotAWarning);
  handleDiagnostic(Diag);
  llvm_unreachable("snippy::fatal should never return");
}

[[noreturn]] void fatal(const llvm::Twine &Prefix, Error E) {
  SnippyDiagnosticInfo Diag(Prefix, toString(std::move(E)), llvm::DS_Error,
                            WarningName::NotAWarning);
  handleDiagnostic(Diag);
  llvm_unreachable("snippy::fatal should never return");
}

} // namespace snippy
} // namespace llvm
