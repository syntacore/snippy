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
#include "snippy/Config/MemoryScheme.h"

namespace llvm {

namespace snippy {

class MonoAllocatableSection {
public:
  MonoAllocatableSection(SectionDesc Section)
      : Desc(std::move(Section)), CurrentOffset(Section.VMA){};

  Expected<uint64_t> getNext(uint64_t Size, uint64_t Alignment) {
    auto NextOffset = alignTo(CurrentOffset, Alignment);
    CurrentOffset = NextOffset + Size;
    if (CurrentOffset > Desc.VMA + Desc.Size) {
      auto Overflow = CurrentOffset - (Desc.VMA + Desc.Size);
      return make_error<Failure>("Out of space when allocating " + Twine(Size) +
                                 " bytes (align " + Twine(Alignment) +
                                 ") in section '" + Desc.getIDString() +
                                 "' of size " + Twine(Desc.Size) + ". " +
                                 Twine(Overflow) + " bytes overflow");
    }
    return NextOffset;
  }

  uint64_t allocatedSize() const { return CurrentOffset - Desc.VMA; }

  virtual ~MonoAllocatableSection() = default;

private:
  SectionDesc Desc;
  uint64_t CurrentOffset;

protected:
  auto &section() const { return Desc; }
};

class GlobalsPool : private MonoAllocatableSection {
public:
  GlobalsPool(LLVMState &State, Module &M, const SectionDesc &Section,
              StringRef Prep, StringRef InputName = "")
      : MonoAllocatableSection(Section), State(State), SectionName(InputName),
        M(M), Prepend(Prep.data()){};

  GlobalVariable *getGV(StringRef Name) {
    return M.getNamedGlobal(Prepend + Name.str());
  }

  GlobalVariable *
  createGV(const APInt &Init, unsigned Alignment = 1u,
           GlobalValue::LinkageTypes Linkage = GlobalValue::InternalLinkage,
           StringRef Name = "global", StringRef Purpose = "",
           bool IsConstant = true) {

    auto Size = Init.getBitWidth() / 8u;
    auto EOffset = getNext(Size, Alignment);
    if (!EOffset) {
      auto E = EOffset.takeError();
      snippy::fatal(
          State.getCtx(),
          [&]() {
            std::string Header;
            raw_string_ostream OS{Header};
            OS << "Failed to create global constant (" << Purpose << ")";
            return Header;
          }(),
          [&]() {
            std::string Msg;
            raw_string_ostream OS{Msg};
            OS << E << "\n";
            OS << "Please, provide more space in specified section or select "
                  "another section to emit global constants to.";
            return Msg;
          }());
    }
    auto &Offset = EOffset.get();
    std::string FinalName = (Prepend + Name).str();
    auto GVAlign = MaybeAlign(Alignment);
    auto *NewGV =
        State.createGlobalConstant(M, Init, Linkage, FinalName, IsConstant);
    NewGV->setAlignment(GVAlign);
    NewGV->setSection(SectionName);
    return GVs.emplace(NewGV, Offset).first->first;
  }

  uint64_t getGVAddress(GlobalVariable *GV) const {
    assert(GVs.count(GV) && "GV is unregistered");
    return GVs.at(GV);
  }

private:
  // Map GV to their address offset
  std::unordered_map<GlobalVariable *, uint64_t> GVs;
  LLVMState &State;
  std::string SectionName;
  Module &M;
  std::string Prepend;
};

struct PGSRegLoc {
  MemAddr Global, Local;
};

class ProgramGlobalStateKeeper : private MonoAllocatableSection {
public:
  ProgramGlobalStateKeeper(LLVMState &State, const SectionDesc &Section)
      : MonoAllocatableSection(Section), State(State){};
  const PGSRegLoc &allocateSaveLocation(MCRegister Reg, size_t RegSize) {
    assert(!SavedRegistersLocations.count(Reg));
    auto getChecked = [&]() {
      auto EOffset = getNext(RegSize, 1u);
      if (!EOffset) {
        auto E = EOffset.takeError();
        snippy::fatal(State.getCtx(),
                      "Failed to create a savepoint for global state", [&]() {
                        std::string Msg;
                        raw_string_ostream OS{Msg};
                        OS << E << "\n";
                        OS << "Please, provide more space for utility section.";
                        return Msg;
                      }());
      }
      return EOffset.get();
    };
    auto LocalOffset = getChecked();
    auto GlobalOffset = getChecked();
    auto Ret = SavedRegistersLocations.emplace(
        Reg, PGSRegLoc{LocalOffset, GlobalOffset});
    return Ret.first->second;
  }

  const PGSRegLoc &getSaveLocation(MCRegister Reg) const {
    return SavedRegistersLocations.at(Reg);
  }

  bool hasSaveLocation(MCRegister Reg) const {
    return SavedRegistersLocations.count(Reg);
  }

private:
  LLVMState &State;
  std::map<MCRegister, PGSRegLoc> SavedRegistersLocations;
};

} // namespace snippy
} // namespace llvm
