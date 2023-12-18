//===-- IntervalsToVerify.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/IntervalsToVerify.h"
#include "snippy/Support/Utils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/YAMLTraits.h"

#define DEBUG_TYPE "snippy-prologue-epilogue-finder"

namespace llvm {

using Interval = snippy::IntervalsToVerify::Interval;

namespace {
struct HexInterval {
  HexInterval(yaml::IO &IO) {}
  HexInterval(yaml::IO &IO, Interval I) : Values{I.First, I.Last} {}

  Interval denormalize(yaml::IO &IO) {
    if (Values.size() != 2)
      IO.setError("Invalid interval: number of fields should be equal to 2");
    return Interval{Values[0], Values[1]};
  }

  SmallVector<yaml::Hex64> Values;
};
} // namespace

template <> struct yaml::MappingTraits<snippy::IntervalsToVerify> {
  static void mapping(yaml::IO &IO, snippy::IntervalsToVerify &Intervals) {
    IO.mapRequired("intervals-to-verify", Intervals.ToVerify);
  }
};

template <>
void yaml::yamlize(IO &IO, Interval &Options, bool, EmptyContext &Ctx) {
  MappingNormalization<HexInterval, Interval> MappingNorm(IO, Options);
  yamlize(IO, MappingNorm->Values, true /* not used */, Ctx);
}

template <> struct yaml::SequenceTraits<HexInterval> {
  static const bool flow = true;
  static size_t size(yaml::IO &IO, HexInterval &List) {
    return List.Values.size();
  }

  static Hex64 &element(yaml::IO &IO, HexInterval &List, size_t I) {
    if (List.Values.size() >= I)
      List.Values.resize(I + 1);
    return List.Values[I];
  }
};

} // namespace llvm

LLVM_YAML_IS_SEQUENCE_VECTOR(llvm::snippy::IntervalsToVerify::Interval)

namespace llvm {
namespace {

class IntervalsToVerifyFinder final {
public:
  struct ObjectContents {
    std::unique_ptr<MemoryBuffer> MemBuf;
    std::unique_ptr<object::ObjectFile> Obj;
    object::SymbolRef EntrySym;
    object::SectionRef TextSection;
    StringRef TextSectionContents;
    uint64_t EntryAddress;
  };

private:
  IntervalsToVerifyFinder(MCDisassembler &D, size_t PrologueInstrCnt,
                          size_t EpilogueInstrCnt, uint64_t SectionVMA);

  Expected<uint64_t> getNthInstrAddress(size_t Num) const;
  Expected<uint64_t> getLastNthInstrAddress(size_t Num) const;
  Error fillIntervals(StringRef ObjectBytes, StringRef EntryPointName);
  Error writeInstructionAddresses(uint64_t StartAddr);

  uint64_t findEndAddressOfEnclosingFunction(uint64_t Addr) const;
  Error fillStartAddressesOfFunctions();

  uint64_t applyVMAOffset(uint64_t Addr) const { return Addr + SectionVMA; }

  // Get a closed interval of Addresses [First, Last] starting with
  // FirstInstrNum-nth address in InstructionAddresses and ending with
  // LastInstrNum-nth address from the end of InstructionAddresses
  Expected<Interval> getEnclosedInterval(size_t FirstInstrNum,
                                         size_t LastInstrNum) const;

public:
  static Expected<IntervalsToVerifyFinder>
  createFromObject(MCDisassembler &D, StringRef ObjectBytes,
                   StringRef EntryPointName, uint64_t SectionVMA,
                   size_t PrologueInstrCnt, size_t EpilogueInstrCnt);

  std::vector<Interval> getIntervalsToVerify() const { return ToVerify; }

private:
  MCDisassembler &Disas;
  ObjectContents TheObjContents;
  std::vector<uint64_t> InstructionAddresses;
  size_t PrologueSZ;
  size_t EpilogueSZ;
  uint64_t SectionVMA;
  std::vector<Interval> ToVerify;
  std::vector<uint64_t> FunctionAddresses;
};

} // namespace

namespace {

Expected<IntervalsToVerifyFinder> IntervalsToVerifyFinder::createFromObject(
    MCDisassembler &D, StringRef ObjectBytes, StringRef EntryPointName,
    uint64_t MappedSectionAddr, size_t PrologueInstrCnt,
    size_t EpilogueInstrCnt) {
  IntervalsToVerifyFinder Finder(D, PrologueInstrCnt, EpilogueInstrCnt,
                                 MappedSectionAddr);
  if (auto E = Finder.fillIntervals(ObjectBytes, EntryPointName))
    return E;
  return Finder;
}

Error IntervalsToVerifyFinder::fillStartAddressesOfFunctions() {
  // Find all function symbols that belong to the same section as the entry
  // point.
  for (const object::SymbolRef &F : TheObjContents.Obj->symbols()) {
    if (!TheObjContents.TextSection.containsSymbol(F))
      continue;

    if (Expected Type = F.getType();
        !Type || *Type != object::SymbolRef::ST_Function)
      continue;

    Expected Addr = F.getAddress();
    if (!Addr)
      return Addr.takeError();
    FunctionAddresses.push_back(*Addr);
  }
  // Sort for binary searches
  sort(FunctionAddresses);
  return Error::success();
}

IntervalsToVerifyFinder::IntervalsToVerifyFinder(MCDisassembler &D,
                                                 size_t PrologueInstrCnt,
                                                 size_t EpilogueInstrCnt,
                                                 uint64_t MappedSectionAddr)
    : Disas(D), PrologueSZ(PrologueInstrCnt), EpilogueSZ(EpilogueInstrCnt),
      SectionVMA(MappedSectionAddr) {}

Error IntervalsToVerifyFinder::writeInstructionAddresses(uint64_t StartAddr) {
  InstructionAddresses.clear();

  object::SectionRef TextSection = TheObjContents.TextSection;
  StringRef TextSectionContents = TheObjContents.TextSectionContents;
  uint64_t EndAddress = findEndAddressOfEnclosingFunction(StartAddr);
  uint64_t Offset = StartAddr - TextSection.getAddress();
  ArrayRef Bytes =
      ArrayRef(TextSectionContents.bytes_begin(), TextSectionContents.size());

  size_t Size;
  uint64_t I = Offset;
  for (; I < EndAddress; I += Size) {
    ArrayRef ThisBytes = Bytes.slice(I);
    MCInst Inst;
    auto Result = Disas.getInstruction(Inst, Size, ThisBytes, I, nulls());
    if (Result != MCDisassembler::Success || Size == 0) {
      return createStringError(inconvertibleErrorCode(),
                               "Failed to disassemble an instruction");
    }
    InstructionAddresses.push_back(applyVMAOffset(I));
  }

  assert(I == EndAddress &&
         "Something went terribly wrong when disassembling instructions");

  return Error::success();
}

// Note that end address is not inclusive
uint64_t IntervalsToVerifyFinder::findEndAddressOfEnclosingFunction(
    uint64_t Addr) const {
  object::SectionRef TextSection = TheObjContents.TextSection;
  auto EndAddrIt = upper_bound(FunctionAddresses, Addr);
  return (EndAddrIt != FunctionAddresses.end()
              ? *EndAddrIt
              : TextSection.getAddress() + TextSection.getSize());
}

Expected<IntervalsToVerifyFinder::ObjectContents>
extractObjectContents(StringRef ObjectBytes, StringRef EntryPointName) {
  auto MemBuf = MemoryBuffer::getMemBuffer(ObjectBytes, "", false);
  std::unique_ptr<object::ObjectFile> Obj;

  Expected Bin = object::createBinary(*MemBuf);
  if (!Bin)
    return Bin.takeError();

  Obj = std::unique_ptr<object::ObjectFile>(
      cast<object::ObjectFile>(Bin->release()));
  auto EntryIt = find_if(Obj->symbols(),
                         [EntryPointName](const object::SymbolRef &Symbol) {
                           if (auto Name = Symbol.getName())
                             return *Name == EntryPointName;
                           return false;
                         });

  if (EntryIt == Obj->symbol_end())
    return createStringError(
        inconvertibleErrorCode(),
        "Object file does not contain specified entry function");

  Expected TextSection = EntryIt->getSection();
  if (!TextSection)
    return TextSection.takeError();

  Expected EntryAddress = EntryIt->getAddress();
  if (!EntryAddress)
    return EntryAddress.takeError();

  Expected SectionContents = TextSection.get()->getContents();
  if (!SectionContents)
    return SectionContents.takeError();

  return IntervalsToVerifyFinder::ObjectContents{
      std::move(MemBuf), std::move(Obj),   *EntryIt,
      **TextSection,     *SectionContents, *EntryAddress};
}

Error IntervalsToVerifyFinder::fillIntervals(StringRef ObjectBytes,
                                             StringRef EntryPointName) {
  Expected ObjectContents = extractObjectContents(ObjectBytes, EntryPointName);
  if (!ObjectContents)
    return ObjectContents.takeError();
  TheObjContents = std::move(*ObjectContents);

  if (Error Err = fillStartAddressesOfFunctions())
    return Err;

  for (uint64_t FuncAddr : FunctionAddresses) {
    bool IsEntry = (FuncAddr == TheObjContents.EntryAddress);
    size_t FirstInstrNum = IsEntry ? PrologueSZ : 0;
    size_t LastInstrNum = IsEntry ? EpilogueSZ : 0;

    if (auto DisasErr = writeInstructionAddresses(FuncAddr))
      return DisasErr;

    Expected Interval = getEnclosedInterval(FirstInstrNum, LastInstrNum);
    if (!Interval)
      return Interval.takeError();

    ToVerify.push_back(*Interval);
  }

  return Error::success();
}

Expected<uint64_t>
IntervalsToVerifyFinder::getNthInstrAddress(size_t Num) const {
  if (Num >= InstructionAddresses.size())
    return createStringError(
        inconvertibleErrorCode(),
        "Did not record an expected amount of instructions");
  return InstructionAddresses[Num];
}

Expected<uint64_t>
IntervalsToVerifyFinder::getLastNthInstrAddress(size_t Num) const {
  if (Num >= InstructionAddresses.size())
    return createStringError(
        inconvertibleErrorCode(),
        "Did not record an expected amount of instructions");
  return *std::next(InstructionAddresses.rbegin(), Num);
}

Expected<Interval>
IntervalsToVerifyFinder::getEnclosedInterval(size_t FirstInstrNum,
                                             size_t LastInstrNum) const {
  Expected First = getNthInstrAddress(FirstInstrNum);
  Expected Last = getLastNthInstrAddress(LastInstrNum);

  if (First && Last)
    return Interval{*First, *Last};

  return joinErrors(First.takeError(), Last.takeError());
}

} // namespace

namespace snippy {

Expected<IntervalsToVerify> IntervalsToVerify::createFromObject(
    MCDisassembler &D, StringRef ObjectBytes, StringRef EntryPointName,
    uint64_t SectionVMA, size_t PrologueInstrCnt, size_t EpilogueInstrCnt) {
  auto Finder = IntervalsToVerifyFinder::createFromObject(
      D, ObjectBytes, EntryPointName, SectionVMA, PrologueInstrCnt,
      EpilogueInstrCnt);

  if (!Finder)
    return Finder.takeError();

  return IntervalsToVerify(Finder->getIntervalsToVerify());
}

void IntervalsToVerify::dumpAsYaml(raw_ostream &OS) {
  yaml::Output Yout(OS);
  Yout << *this;
}

Error IntervalsToVerify::dumpAsYaml(StringRef Filename) {
  return checkedWriteToOutput(Filename, [this](raw_ostream &OS) {
    dumpAsYaml(OS);
    return Error::success();
  });
}

} // namespace snippy
} // namespace llvm
