//===-- ParsedElf.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include "snippy/Generator/Linker.h"
#include "snippy/Simulator/Types.h"

namespace llvm {

namespace snippy {

class ParsedElf final {
  using SectionFilterPFN = std::function<bool(llvm::object::SectionRef)>;

  std::unique_ptr<MemoryBuffer> MemBuf;
  std::unique_ptr<object::ObjectFile> ObjectFile;

  ParsedElf(std::unique_ptr<MemoryBuffer> &&MemBuf,
            std::unique_ptr<object::ObjectFile> &&ObjectFile,
            SmallVector<object::SectionRef> &&AllocatableSections,
            SmallVector<object::SectionRef> &&TextAndDataSections,
            SmallVector<object::SectionRef> &&BSSSections,
            ProgramCounterType ProgStart,
            std::optional<ProgramCounterType> ProgEnd)
      : MemBuf(std::move(MemBuf)), ObjectFile(std::move(ObjectFile)),
        AllocatableSections(std::move(AllocatableSections)),
        TextAndDataSections(std::move(TextAndDataSections)),
        BSSSections(std::move(BSSSections)), ProgStart(ProgStart),
        ProgEnd(ProgEnd) {}

public:
  bool isObjectFileRelocatable() const {
    return ObjectFile->isRelocatableObject();
  }

  static SmallVector<object::SectionRef>
  getAllocatableSectionsFromImage(object::ObjectFile &ObjectFile,
                                  SectionFilterPFN SectionFilter) {
    SmallVector<object::SectionRef> AllocatableSections;

    llvm::copy_if(ObjectFile.sections(),
                  std::back_inserter(AllocatableSections), [&](auto &&Section) {
                    return (!SectionFilter || SectionFilter(Section)) &&
                           (Section.isText() || Section.isData() ||
                            Section.isBSS());
                  });

    return AllocatableSections;
  }

  static SmallVector<object::SectionRef> getTextAndDataSectionsFromImage(
      const SmallVector<object::SectionRef> &Sections,
      object::ObjectFile &ObjectFile) {
    SmallVector<object::SectionRef> TextAndDataSections;

    llvm::copy(make_filter_range(Sections,
                                 [&](auto &&Section) {
                                   return Section.isText() || Section.isData();
                                 }),
               std::back_inserter(TextAndDataSections));

    return TextAndDataSections;
  }

  static SmallVector<object::SectionRef>
  getBSSSectionsFromImage(const SmallVector<object::SectionRef> &Sections,
                          object::ObjectFile &ObjectFile) {
    SmallVector<object::SectionRef> BSSSections;

    llvm::copy(make_filter_range(
                   Sections, [&](auto &&Section) { return Section.isBSS(); }),
               std::back_inserter(BSSSections));

    return BSSSections;
  }

  static Expected<ProgramCounterType>
  getProgStartFromImage(object::ObjectFile &ObjectFile,
                        StringRef EntryPointSymbol) {
    auto FindEntryPoint = [&]() -> Expected<ProgramCounterType> {
      assert(!EntryPointSymbol.empty());
      auto EntryPointSym = llvm::find_if(ObjectFile.symbols(), [&](auto &Sym) {
        auto EName = Sym.getName();
        return EName && EName.get() == EntryPointSymbol;
      });
      if (EntryPointSym == ObjectFile.symbols().end())
        return createStringError(
            makeErrorCode(Errc::CorruptedElfImage),
            formatv("Elf does not have specified entry point name '{0}'",
                    EntryPointSymbol));

      return EntryPointSym->getAddress();
    };

    return !EntryPointSymbol.empty() ? FindEntryPoint()
                                     : ObjectFile.getStartAddress();
  }

  static Expected<std::optional<ProgramCounterType>>
  getProgEndFromImage(object::ObjectFile &ObjectFile) {
    auto EndOfProgSym = llvm::find_if(ObjectFile.symbols(), [](auto &Sym) {
      auto EName = Sym.getName();
      return EName && EName.get() == Linker::getExitSymbolName();
    });

    if (EndOfProgSym == ObjectFile.symbols().end())
      return std::nullopt;

    return EndOfProgSym->getAddress();
  }

  static Expected<ParsedElf>
  createParsedElf(StringRef ElfImage, StringRef EntryPointSymbol = "",
                  SectionFilterPFN SectionFilter = nullptr) {
    auto MemBuf = MemoryBuffer::getMemBuffer(ElfImage, "", false);
    auto ObjectFile = object::ObjectFile::createObjectFile(*MemBuf);
    if (!ObjectFile)
      return ObjectFile.takeError();

    auto AllocatableSections =
        getAllocatableSectionsFromImage(*ObjectFile.get(), SectionFilter);

    auto TextAndDataSections =
        getTextAndDataSectionsFromImage(AllocatableSections, *ObjectFile.get());
    auto BSSSections =
        getBSSSectionsFromImage(AllocatableSections, *ObjectFile.get());

    auto ProgStart = getProgStartFromImage(*ObjectFile.get(), EntryPointSymbol);
    if (!ProgStart)
      return ProgStart.takeError();

    auto ProgEnd = getProgEndFromImage(*ObjectFile.get());
    if (!ProgEnd)
      return ProgEnd.takeError();

    return ParsedElf(std::move(MemBuf), std::move(ObjectFile.get()),
                     std::move(AllocatableSections),
                     std::move(TextAndDataSections), std::move(BSSSections),
                     *ProgStart, *ProgEnd);
  }

  const SmallVector<object::SectionRef> AllocatableSections;
  const SmallVector<object::SectionRef> TextAndDataSections;
  const SmallVector<object::SectionRef> BSSSections;

  ProgramCounterType ProgStart;
  std::optional<ProgramCounterType> ProgEnd;
};

} // namespace snippy
} // namespace llvm
