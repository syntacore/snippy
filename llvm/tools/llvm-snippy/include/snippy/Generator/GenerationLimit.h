//===-- GenerationLimit.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GENERATION_LIMIT_H
#define LLVM_TOOLS_SNIPPY_GENERATION_LIMIT_H

#include "snippy/Support/Utils.h"

#include <variant>

namespace llvm {
namespace snippy {

// Return how many primary instrs were generated and size of all generated
// instrs
struct GenerationStatistics final {
  size_t NumOfPrimaryInstrs;
  size_t GeneratedSize;
  bool UnableToFitAnymore;

  GenerationStatistics(size_t NumOfPrimaryInstrs = 0, size_t GeneratedSize = 0,
                       bool UnableToFitAnymore = false)
      : NumOfPrimaryInstrs(NumOfPrimaryInstrs), GeneratedSize(GeneratedSize),
        UnableToFitAnymore(UnableToFitAnymore) {}

  void merge(const GenerationStatistics &Statistics) {
    NumOfPrimaryInstrs += Statistics.NumOfPrimaryInstrs;
    GeneratedSize += Statistics.GeneratedSize;
    UnableToFitAnymore |= Statistics.UnableToFitAnymore;
  }

  void print(raw_ostream &OS) const {
    OS << "GenStats={ "
       << "NumOfPrimaryInstrs: " << NumOfPrimaryInstrs
       << ", GeneratedSize: " << GeneratedSize
       << ", UnableToFitAnymore: " << UnableToFitAnymore << " }";
  }
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

namespace planning {

class RequestLimit final {
public:
  struct NumInstrs final {
    size_t Limit;
  };
  struct Size final {
    size_t Limit;
  };
  struct Mixed final {
    size_t NumInstrs;
    size_t Size;
  };

private:
  std::variant<NumInstrs, Size, Mixed> Limit;

public:
  template <typename LimitTy> RequestLimit(LimitTy Lim) : Limit(Lim) {}

  bool isSizeLimit() const { return std::holds_alternative<Size>(Limit); }

  bool isNumLimit() const { return std::holds_alternative<NumInstrs>(Limit); }

  bool isMixedLimit() const { return std::holds_alternative<Mixed>(Limit); }

  bool isSameKindAs(const RequestLimit &Other) const {
    return Limit.index() == Other.Limit.index();
  }

  bool isEmpty() const;

  std::string getAsString() const;

  RequestLimit &operator+=(const RequestLimit &Other);

  bool operator==(const RequestLimit &Other) const;

  size_t getLimit() const;

  RequestLimit::Mixed getMixedLimit() const;

  size_t getNumInstrsLeft(GenerationStatistics Stats) const;

  size_t getSizeLeft(GenerationStatistics Stats) const;

  bool isReached(GenerationStatistics Stats) const;
};
} // namespace planning
} // namespace snippy
} // namespace llvm
#endif
