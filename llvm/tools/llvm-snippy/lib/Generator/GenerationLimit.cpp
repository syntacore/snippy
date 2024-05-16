//===-- GenerationLimit.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationLimit.h"
#include "snippy/Support/DiagnosticInfo.h"

namespace llvm {
namespace snippy {
namespace planning {
std::string RequestLimit::getAsString() const {
  return std::visit(OverloadedCallable(
                        [](const NumInstrs &Lim) -> std::string {
                          std::string Str;
                          raw_string_ostream SS(Str);
                          SS << "NumInstr (Max: " << Lim.Limit << ")";
                          return SS.str();
                        },
                        [](const Size &Lim) -> std::string {
                          std::string Str;
                          raw_string_ostream SS(Str);
                          SS << "Size (Max: " << Lim.Limit << ")";
                          return Str;
                        },
                        [](const Mixed &Lim) -> std::string {
                          std::string Str;
                          raw_string_ostream SS(Str);
                          SS << "Mixed (MaxNumInstr: " << Lim.NumInstrs
                             << ", MaxSize: " << Lim.Size << ")";
                          return Str;
                        }),
                    Limit);
}

bool RequestLimit::isReached(GenerationStatistics Stats) const {
  return std::visit(OverloadedCallable(
                        [&Stats](const NumInstrs &Lim) {
                          return Stats.NumOfPrimaryInstrs >= Lim.Limit;
                        },
                        [&Stats](const Size &Lim) {
                          return Stats.GeneratedSize >= Lim.Limit;
                        },
                        [&Stats](const Mixed &Lim) {
                          return Stats.GeneratedSize >= Lim.Size ||
                                 Stats.NumOfPrimaryInstrs >= Lim.NumInstrs;
                        }),
                    Limit);
}

size_t RequestLimit::getSizeLeft(GenerationStatistics Stats) const {
  return std::visit(OverloadedCallable(
                        [&Stats](const Size &Lim) -> size_t {
                          return Lim.Limit >= Stats.GeneratedSize
                                     ? Lim.Limit - Stats.GeneratedSize
                                     : 0;
                        },
                        [&Stats](const Mixed &Lim) -> size_t {
                          return Lim.Size >= Stats.GeneratedSize
                                     ? Lim.Size - Stats.GeneratedSize
                                     : 0;
                        },
                        [](const NumInstrs &) -> size_t {
                          llvm_unreachable("Calling getSizeLeft on NumInstrs "
                                           "limit does not make sense.");
                        }),
                    Limit);
}
size_t RequestLimit::getNumInstrsLeft(GenerationStatistics Stats) const {
  return std::visit(OverloadedCallable(
                        [&Stats](const NumInstrs &Lim) -> size_t {
                          return Lim.Limit >= Stats.NumOfPrimaryInstrs
                                     ? Lim.Limit - Stats.NumOfPrimaryInstrs
                                     : 0;
                        },
                        [&Stats](const Mixed &Lim) -> size_t {
                          return Lim.NumInstrs >= Stats.NumOfPrimaryInstrs
                                     ? Lim.NumInstrs - Stats.NumOfPrimaryInstrs
                                     : 0;
                        },
                        [](const Size &) -> size_t {
                          llvm_unreachable("Calling getNumInstrsLeft on size "
                                           "limit does not make sense.");
                        }),
                    Limit);
}

RequestLimit &RequestLimit::operator+=(const RequestLimit &Other) {
  if (!isMixedLimit() && !isSameKindAs(Other)) {
    if (getLimit() == 0) { // empty
      Limit = Other.Limit; // overwrite
      return *this;
    }
    LLVMContext Ctx;
    snippy::fatal(
        Ctx,
        "Attempt to add limit <" + Twine(Other.getAsString()) + "> to limit <" +
            Twine(getAsString()) + ">",
        "You can only sum limits in one of the following ways: Limits of "
        "the same kind, \"Mixed\" with any or empty \"non-Mixed\" with any");
  }
  std::visit(OverloadedCallable(
                 [&Other](NumInstrs &Lim) { Lim.Limit += Other.getLimit(); },
                 [&Other](Size &Lim) { Lim.Limit += Other.getLimit(); },
                 [&Other](Mixed &Lim) {
                   if (Other.isSizeLimit()) {
                     Lim.Size += Other.getLimit();
                   } else if (Other.isNumLimit()) {
                     Lim.NumInstrs += Other.getLimit();
                   } else if (Other.isMixedLimit()) {
                     auto &MixedOther = std::get<Mixed>(Other.Limit);
                     Lim.NumInstrs += MixedOther.NumInstrs;
                     Lim.Size += MixedOther.Size;
                   } else {
                     llvm_unreachable("Unknown limit kind");
                   }
                 }),
             Limit);
  return *this;
}

size_t RequestLimit::getLimit() const {
  return std::visit(
      OverloadedCallable(
          [](const NumInstrs &Limit) -> size_t { return Limit.Limit; },
          [](const Size &Limit) -> size_t { return Limit.Limit; },
          [](const Mixed &) -> size_t {
            llvm_unreachable(
                "Calling getLimit on mixed limit does not make sense.");
          }),
      Limit);
}
} // namespace planning
} // namespace snippy
} // namespace llvm
