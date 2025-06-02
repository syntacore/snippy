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

#define FOR_ALL_WARNINGS(WARN_CASE)                                            \
  WARN_CASE(NotAWarning, "not-a-warning")                                      \
  WARN_CASE(MemoryAccess, "memory-access")                                     \
  WARN_CASE(NoModelExec, "no-model-exec")                                      \
  WARN_CASE(InstructionHistogram, "instruction-histogram")                     \
  WARN_CASE(RelocatableGenerated, "relocatable-generated")                     \
  WARN_CASE(InconsistentOptions, "inconsistent-options")                       \
  WARN_CASE(LoopIterationNumber, "loop-iteration-number")                      \
  WARN_CASE(LoopCounterOutOfRange, "loop-counter-out-of-range")                \
  WARN_CASE(BurstMode, "burst-mode")                                           \
  WARN_CASE(InstructionCount, "instruction-count")                             \
  WARN_CASE(RegState, "register-state")                                        \
  WARN_CASE(InstructionSizeUnknown, "instruction-size-unknown")                \
  WARN_CASE(TooFarMaxPCDist, "too-long-max-pc-dist")                           \
  WARN_CASE(ModelException, "model-exception")                                 \
  WARN_CASE(UnusedSection, "unused-section")                                   \
  WARN_CASE(EmptyElfSection, "empty-elf-section")                              \
  WARN_CASE(GenPlanVerification, "gen-plan-verification")                      \
  WARN_CASE(SeedNotSpecified, "seed-not-specified")                            \
  WARN_CASE(NonReproducibleExecution, "non-reproducible-execution")            \
  WARN_CASE(MArchIsTriple, "march-is-triple")

#ifdef WARN_CASE
#error WARN_CASE should not be defined at this point
#else
#define WARN_CASE(NAME, STR) NAME,
#endif
enum class WarningName { FOR_ALL_WARNINGS(WARN_CASE) };
#undef WARN_CASE

template <WarningName W> constexpr inline StringLiteral WarningNameOf = "";
#ifdef WARN_CASE
#error WARN_CASE should not be defined at this point
#else
#define WARN_CASE(NAME, STR)                                                   \
  template <>                                                                  \
  constexpr inline StringLiteral WarningNameOf<WarningName::NAME> = STR;
#endif
FOR_ALL_WARNINGS(WARN_CASE)
#undef WARN_CASE

std::optional<WarningName> getWarningName(StringRef Warn);

StringLiteral getWarningNameStr(WarningName Warn);

void checkWarningOptions();

struct WarningCounters {
  size_t EncounteredOrder;
  size_t EncounteredTotal;
};

class SnippyDiagnosticInfo : public llvm::DiagnosticInfo {

  static const int KindID;
  std::string Description;
  WarningName WName;

  static int getKindID() { return KindID; }
  static std::map<std::string, WarningCounters> ReportedWarnings;

public:
  SnippyDiagnosticInfo(const llvm::Twine &Desc,
                       llvm::DiagnosticSeverity SeverityIn, WarningName WNameIn)
      : llvm::DiagnosticInfo(getKindID(), SeverityIn), Description(Desc.str()),
        WName(WNameIn) {}

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

void notice(const llvm::Twine &Prefix, const llvm::Twine &Desc,
            WarningName WN = WarningName::NotAWarning);

void notice(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
            const llvm::Twine &Desc, WarningName WN = WarningName::NotAWarning);

void warn(WarningName WN, llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
          const llvm::Twine &Desc);

void warn(WarningName WN, const llvm::Twine &Prefix, const llvm::Twine &Desc);

[[noreturn]] void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
                        const llvm::Twine &Desc);

[[noreturn]] void fatal(llvm::LLVMContext &Ctx, const llvm::Twine &Prefix,
                        Error E);

[[noreturn]] void fatal(Error E);

[[noreturn]] void fatal(const llvm::Twine &Prefix, Error E);

[[noreturn]] void fatal(const llvm::Twine &Prefix, const llvm::Twine &Desc);

[[noreturn]] void fatal(const llvm::Twine &Desc);

/// \brief Unwrap Expected value or fail with fatal error.
template <typename T> T unwrapOrFatal(Expected<T> ValOrErr) {
  if (!ValOrErr)
    snippy::fatal("Unwrapping 'Expected' value", ValOrErr.takeError());
  return std::move(*ValOrErr);
}

} // namespace snippy

} // namespace llvm
