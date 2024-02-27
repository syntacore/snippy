//===-- DiagnostricInfo.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Error.h"

#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/Support/Debug.h"

#include <map>
#include <string>
#include <vector>

namespace llvm {

class DiagnosticPrinter;

namespace snippy {

enum class WarningName {
  NotAWarning,
  MemoryAccess,
  NoModelExec,
  InstructionHistogram,
  RelocatableGenerated,
  InconsistentOptions,
  LoopIterationNumber,
  BurstMode,
  InstructionCount,
  BranchegramOverride,
  RegState,
  InstructionSizeUnknown,
  TooFarMaxPCDist,
  ModelException,
  UnusedSection
};

struct WarningCounters {
  size_t EncounteredOrder;
  size_t EncounteredTotal;
};

class SnippyDiagnosticInfo : public llvm::DiagnosticInfo {

  static const int KindID;
  std::string Description;
  llvm::DiagnosticSeverity Severity;
  WarningName WName;

  static int getKindID() { return KindID; }
  static std::map<std::string, WarningCounters> ReportedWarnings;

public:
  SnippyDiagnosticInfo(const llvm::Twine &Desc,
                       llvm::DiagnosticSeverity SeverityIn, WarningName WNameIn)
      : llvm::DiagnosticInfo(getKindID(), SeverityIn), Description(Desc.str()),
        Severity(SeverityIn), WName(WNameIn) {}

  SnippyDiagnosticInfo(const llvm::Twine &Prefix, const llvm::Twine &Desc,
                       llvm::DiagnosticSeverity SeverityIn, WarningName WNameIn)
      : SnippyDiagnosticInfo(Prefix + ": " + Desc, SeverityIn, WNameIn) {}

  void print(llvm::DiagnosticPrinter &DP) const override;

  WarningName getName() const { return WName; }

  static bool classof(const llvm::DiagnosticInfo *DI) {
    return DI->getKind() == getKindID();
  }

  /* the first element of the pair contains a warning, the second - the number
   * this warning was emitted */
  static std::vector<std::pair<std::string, size_t>> fetchReportedWarnings();
};

void notice(WarningName WN, llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
            const llvm::Twine &Desc);

void warn(WarningName WN, llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
          const llvm::Twine &Desc);

void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
           const llvm::Twine &Desc);

void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix, Error E);

} // namespace snippy

} // namespace llvm
