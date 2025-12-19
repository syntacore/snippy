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

bool RequestLimit::isEmpty() const {
  return std::visit(
      OverloadedCallable(
          [](const auto &Lim) { return Lim.Limit == 0; },
          [](const Mixed &Lim) { return Lim.Size == 0 && Lim.NumInstrs == 0; }),
      Limit);
}

bool RequestLimit::isReached(GenerationStatistics Stats) const {
  return std::visit(OverloadedCallable(
                        [&Stats](const NumInstrs &Lim) {
                          return Stats.NumOfPrimaryInstrs >= Lim.Limit ||
                                 Stats.UnableToFitAnymore;
                        },
                        [&Stats](const Size &Lim) {
                          return Stats.GeneratedSize >= Lim.Limit ||
                                 Stats.UnableToFitAnymore;
                        },
                        [&Stats](const Mixed &Lim) {
                          return Stats.GeneratedSize >= Lim.Size ||
                                 Stats.NumOfPrimaryInstrs >= Lim.NumInstrs ||
                                 Stats.UnableToFitAnymore;
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
  if (isSameKindAs(Other)) {
    std::visit(OverloadedCallable(
                   [&Other](NumInstrs &Lim) { Lim.Limit += Other.getLimit(); },
                   [&Other](Size &Lim) { Lim.Limit += Other.getLimit(); },
                   [&Other](Mixed &Lim) {
                     auto &MixedOther = std::get<Mixed>(Other.Limit);
                     Lim.NumInstrs += MixedOther.NumInstrs;
                     Lim.Size += MixedOther.Size;
                   }),
               Limit);
    return *this;
  }

  if (isEmpty()) {
    Limit = Other.Limit; // overwrite
    return *this;
  }

  auto AddPlainToMixed = [](auto &MixedLimit, auto &PlainLimit) {
    if (PlainLimit.isNumLimit())
      MixedLimit.NumInstrs += PlainLimit.getLimit();
    else if (PlainLimit.isSizeLimit())
      MixedLimit.Size += PlainLimit.getLimit();
    else
      llvm_unreachable("Plain limit has undefined kind");
    return MixedLimit;
  };

  if (Other.isMixedLimit()) {
    RequestLimit::Mixed ResLimit(std::get<Mixed>(Other.Limit));
    Limit = AddPlainToMixed(ResLimit, *this);
    return *this;
  }

  if (isMixedLimit()) {
    RequestLimit::Mixed &CurLimit = std::get<Mixed>(Limit);
    AddPlainToMixed(CurLimit, Other);
    return *this;
  }

  auto NumLimit = Other.isNumLimit() ? Other.getLimit() : getLimit();
  auto SizeLimit = Other.isSizeLimit() ? Other.getLimit() : getLimit();
  Limit = RequestLimit::Mixed{NumLimit, SizeLimit};
  return *this;
}

// Returns false if the limits are not the same kind
bool RequestLimit::operator==(const RequestLimit &Other) const {
  if (!isSameKindAs(Other))
    return false;

  if (!isMixedLimit())
    return getLimit() == Other.getLimit();

  return getMixedLimit().NumInstrs == Other.getMixedLimit().NumInstrs &&
         getMixedLimit().Size == Other.getMixedLimit().Size;
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

RequestLimit::Mixed RequestLimit::getMixedLimit() const {
  return std::visit(
      OverloadedCallable(
          [](const NumInstrs &) -> Mixed {
            llvm_unreachable("Calling getMixedLimit on NumInstrs limit does "
                             "not make sense.");
          },
          [](const Size &) -> Mixed {
            llvm_unreachable(
                "Calling getMixedLimit on size limit does not make sense.");
          },
          [](const Mixed &Limit) -> Mixed { return Limit; }),
      Limit);
}

} // namespace planning
} // namespace snippy
} // namespace llvm
