//===-- GlobalsPool.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// GlobalsPool is used to store info about global variables initialized
/// directly from binary image.
///
///
//===----------------------------------------------------------------------===//

#pragma once

#include "LLVMState.h"

namespace llvm {

namespace snippy {

class GlobalsPool {
public:
  GlobalsPool(LLVMState &State, Module &M, uint64_t SectionAddr,
              uint64_t MaxSize, StringRef Prepend = "",
              StringRef SectionName = "")
      : SectionAddr(SectionAddr), SectionName(SectionName), MaxSize(MaxSize),
        State(State), M(M), Prepend(Prepend){};

  const GlobalVariable *getGV(StringRef Name) { return M.getNamedGlobal(Name); }

  const GlobalVariable *
  createGV(const APInt &Init, unsigned Alignment = 1u,
           GlobalValue::LinkageTypes Linkage = GlobalValue::InternalLinkage,
           StringRef Name = "global", StringRef Reason = "",
           bool IsConstant = true) {

    if (MaxSize == 0)
      snippy::fatal(State.getCtx(), "Failed to create global constant",
                    "No read-only ('r') section provided in layout. " + Reason);
    auto Size = Init.getBitWidth() / 8u;
    auto Offset = alignTo(TotalSize, Alignment);
    TotalSize = Offset + Size;
    if (TotalSize > MaxSize)
      snippy::fatal(State.getCtx(), "Failed to create global constant",
                    " read-only ('r') section overflow; "
                    "Please provide more space in ('r') section in your layout "
                    "configuration. " +
                        Reason);
    std::string FinalName = (Prepend + Name).str();
    auto GVAlign = MaybeAlign(Alignment);
    auto NewGV =
        State.createGlobalConstant(M, Init, Linkage, FinalName, IsConstant);
    NewGV->setAlignment(GVAlign);
    if (!SectionName.empty())
      NewGV->setSection(SectionName);
    return GVs.emplace(NewGV, Offset).first->first;
  }

  uint64_t getGVAddress(const GlobalVariable *GV) const {
    assert(GVs.count(GV) && "GV is unregistered");
    return GVs.at(GV) + SectionAddr;
  }

  uint64_t totalSectionSize() const { return TotalSize; }

  static constexpr char GlobalsSectionName[] = ".snippydata";

private:
  // Map GV to their address offset
  std::unordered_map<const GlobalVariable *, unsigned> GVs;
  const uint64_t SectionAddr;
  std::string SectionName;
  uint64_t TotalSize = 0ull;
  const uint64_t MaxSize;
  LLVMState &State;
  Module &M;
  std::string Prepend;
};

} // namespace snippy
} // namespace llvm
