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

namespace llvm {

// The unique option because it used not only in snippy
cl::OptionCategory DiagnosticOptions("llvm-diagnostic");

namespace snippy {

static snippy::opt<bool>
    WError("werror", cl::desc("All warnings would be treated as errors"),
           cl::cat(llvm::DiagnosticOptions), cl::init(false));

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

void SnippyDiagnosticInfo::print(llvm::DiagnosticPrinter &DP) const {
  DP << Description;

  if (Severity == llvm::DS_Error)
    return;

  if (Severity >= llvm::DS_Remark)
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
  auto MsgCategory = WError ? llvm::DS_Error : llvm::DS_Warning;
  SnippyDiagnosticInfo Diag(Prefix, Desc, MsgCategory, WN);
  handleDiagnostic(Ctx, Diag);
}

void warn(WarningName WN, const llvm::Twine &Prefix, const llvm::Twine &Desc) {
  auto MsgCategory = WError ? llvm::DS_Error : llvm::DS_Warning;
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
