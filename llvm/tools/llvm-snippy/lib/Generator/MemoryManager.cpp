//===-- MemoryManager.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/MemoryManager.h"
#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/FunctionGeneratorPass.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
#include "llvm/Target/TargetLoweringObjectFile.h"

#include <cassert>
#include <vector>

#define DEBUG_TYPE "snippy-memory-manager"

namespace llvm::snippy {

namespace {

MemoryMap getRandomSectionImageForSize(size_t Size) {
  constexpr MemoryUnit MinMemValue = 0u;
  constexpr MemoryUnit MaxMemValue = std::numeric_limits<MemoryUnit>::max();
  auto MemMap = MemoryMap{};
  for (size_t SectOffset = 0; SectOffset < Size;
       SectOffset += sizeof(MemoryUnit)) {
    auto NewCellValue =
        RandEngine::genInRangeInclusive(MinMemValue, MaxMemValue);
    MemMap.emplace(SectOffset, NewCellValue);
  }
  return MemMap;
}

void checkSectionPresence(ArrayRef<SectionData> Sections,
                          const SectionDesc &NewSect) {
  [[maybe_unused]] auto DuplicatedSect =
      std::find_if(Sections.begin(), Sections.end(),
                   [&NewSect](const SectionData &OldSectData) {
                     return NewSect.interfere(OldSectData.Desc);
                   });
  assert(DuplicatedSect == Sections.end() && "Interfering sections");
}

// PrivateLinkage - globals without symbol in symbol table
// InternalLinkage - with global symbol
void createMutableGlobal(GlobalsPool &Emitter, const APInt &Init,
                         size_t Alignment) {
  Emitter.createGV(Init, Alignment, GlobalValue::PrivateLinkage,
                   /*Name*/ "global",
                   /*Reason*/ "Memory initialization",
                   /*Unconstant*/ false);
}

template <typename ItType>
APInt createMemoryRegion(ItType MemBeg, ItType MemEnd,
                         const std::string &SectName = "") {
  assert(sizeof(*MemBeg) == sizeof(MemoryUnit));
  std::vector<APInt::WordType> Memory;
  transformBytesToNumbersArray(MemBeg, MemEnd, std::back_inserter(Memory));
  assert(Memory.size() == (MemEnd - MemBeg) / sizeof(APInt::WordType));
  auto MemoryRealSize = Memory.size() * sizeof(APInt::WordType);
  assert(MemoryRealSize * CHAR_BIT < std::numeric_limits<unsigned>::max());
  return APInt{static_cast<unsigned>(MemoryRealSize * CHAR_BIT),
               ArrayRef<APInt::WordType>(Memory.data(), Memory.size())};
}

void markAccessForSection(SectionData &Sect, MemAddr Addr, size_t AccessSize) {
  assert(Addr >= Sect.Desc.VMA);
  auto LocalAddr = Addr - Sect.Desc.VMA;
  for (MemAddr Offset = 0; Offset < AccessSize; ++Offset)
    Sect.AddressesToInit.insert(LocalAddr + Offset);
}

class FileWrapper {
  struct FilePos {
    SmallVector<StringRef>::iterator LineRef;
    size_t LineNum;
  };

  std::unique_ptr<MemoryBuffer> Data;
  SmallVector<StringRef> Lines;
  const std::string FileName;
  FilePos CurPos;

public:
  FileWrapper(std::unique_ptr<MemoryBuffer> File, StringRef Name)
      : Data(std::move(File)), FileName(Name.str()) {
    assert(Data);
    Data->getBuffer().split(Lines, "\n");
    CurPos = FilePos{Lines.begin(), 1};
  }

  const std::string &getFileName() const { return FileName; }

  std::pair<StringRef, size_t> getCurLine() {
    assert(!isEOF());
    return {*CurPos.LineRef, CurPos.LineNum};
  }

  size_t getLineNum() const { return CurPos.LineNum; }

  bool isEOF() const { return CurPos.LineRef == Lines.end(); }

  void shiftToNextNonEmptyLine() {
    assert(!isEOF());
    do {
      CurPos.LineRef++;
      CurPos.LineNum++;
    } while (CurPos.LineRef->empty() && !isEOF());
  }
};

void reportParsingError(Twine Msg, const FileWrapper &File) {
  auto LineNum = File.getLineNum();
  snippy::fatal(formatv("[{0}, {1}] Wrong memory init file format: {2}",
                        File.getFileName(), LineNum, Msg));
}

template <typename It> SmallVector<StringRef> removeEmptyWords(It Beg, It End) {
  SmallVector<StringRef> Res;
  std::copy_if(Beg, End, std::back_inserter(Res),
               [](StringRef Word) { return !Word.empty(); });
  return Res;
}

// example:
// 0x100: 00 11 22 33 44 55 66 77 88 99 10 11 12 13 14 15
template <typename Inserter>
static bool parseLine(FileWrapper &File, MemAddr &Addr, Inserter Insert) {
  if (File.isEOF())
    return false;

  auto [Line, LineNum] = File.getCurLine();
  // example: 0x1f0:
  Regex LineAddr{"0x([0-9a-f]+):"};
  SmallVector<StringRef> MatchedGroups;

  std::string Error;
  if (!LineAddr.match(Line, &MatchedGroups, &Error))
    return false;

  if (MatchedGroups.size() != 2)
    reportParsingError("can't parse line address", File);

  APInt Res;
  constexpr auto Radix = 16u;
  // This function RETURNS TRUE IF THERE IS NO INTEGER IN THE FRONT
  // StringRef::consume_front() return false on fail - thesis
  // StringRef::consumeInteger() returns true on fail - antithesis
  if (MatchedGroups[1].consumeInteger(Radix, Res))
    reportParsingError("can't convert address to the integer: " +
                           Twine(MatchedGroups[1]),
                       File);

  if (Addr != Res.getLimitedValue())
    reportParsingError("wrong address: " + Twine(MatchedGroups[1]), File);

  SmallVector<StringRef> Words;
  Line.split(Words, " ");
  Words = removeEmptyWords(Words.begin(), Words.end());
  if (Words[0] != MatchedGroups[0])
    reportParsingError("address should be at the beggining", File);

  if (Words.size() <= 1)
    reportParsingError("empty line specified", File);

  std::vector<MemoryUnit> Bytes;
  std::transform(std::next(Words.begin()), Words.end(),
                 std::back_inserter(Bytes), [&File](StringRef Word) {
                   APInt Res;
                   auto WholeWord = Word;
                   // We want Word to be a number without additional symbols
                   assert(Radix == 16u);
                   if (Word.consumeInteger(Radix, Res) || Word.size() != 0)
                     reportParsingError(
                         "can't convert to byte: " + Twine(WholeWord), File);
                   return Res.getLimitedValue();
                 });
  for (auto [Offset, Val] : enumerate(Bytes))
    Insert = {Addr + Offset, Val};
  auto NumOfBytesInLine = Words.size() - 1;
  Addr += NumOfBytesInLine;
  return true;
}

template <typename Inserter>
static bool parseSection(FileWrapper &File, Inserter Insert) {
  if (File.isEOF())
    return false;

  auto [Line, LineNum] = File.getCurLine();
  // example: Section {name1}:
  Regex SectionHeaderRegex{"Section {([A-Za-z0-9_]+)}:$"};
  SmallVector<StringRef> MatchedGrups;

  if (!SectionHeaderRegex.match(Line, &MatchedGrups))
    reportParsingError("can't match section header", File);

  if (MatchedGrups.size() != 2)
    reportParsingError("can't parse section name", File);

  MemoryMap SectData;
  MemAddr Addr = 0ul;
  do {
    File.shiftToNextNonEmptyLine();
  } while (parseLine(File, Addr, std::inserter(SectData, SectData.end())));
  // First match is the whole expression. Second one is a name
  Insert = typename Inserter::container_type::value_type(MatchedGrups[1].str(),
                                                         std::move(SectData));
  return true;
}

SectionDesc getDescription(StringRef Name, GeneratorContext &Ctx) {
  auto Sections = Ctx.getConfig().ProgramCfg.Sections;
  auto SectIt = std::find_if(
      Sections.begin(), Sections.end(),
      [&Name](const SectionDesc &Entry) { return Entry.getName() == Name; });

  if (SectIt == Sections.end())
    snippy::fatal(
        formatv("Can't find section {0} from memory init file", Name));
  return *SectIt;
}

// Input  - pairs: address is section - value
// Output - memory of the section with padding zeros
template <typename It>
std::vector<MemoryUnit> getContinuosSectMemMap(It Beg, It End,
                                               size_t MemMapSize) {
  std::vector<MemoryUnit> ContinMemMap;
  assert(std::is_sorted(Beg, End));
  for (auto [LocalAddr, Val] : make_range(Beg, End)) {
    assert(LocalAddr >= ContinMemMap.size());
    // inserts padding zeros between neighboring input pairs
    ContinMemMap.insert(ContinMemMap.end(), LocalAddr - ContinMemMap.size(),
                        0u);
    ContinMemMap.push_back(Val);
  }
  assert(MemMapSize >= ContinMemMap.size());
  ContinMemMap.insert(ContinMemMap.end(), MemMapSize - ContinMemMap.size(), 0u);
  return ContinMemMap;
}

template <typename It>
SectionData &getSection(size_t Addr, size_t AccessSize, It Beg, It End) {
  auto SectIt = std::find_if(
      Beg, End, [Addr, AccessSize](const SectionData &SectToCheck) {
        return SectToCheck.isAccessValid(Addr, AccessSize);
      });
  assert(SectIt != End && "Memory access out of sections");
  return *SectIt;
}

} // namespace

SectionData
MemInitializationPseudoRandom::getSectionData(const SectionDesc &SectDesc,
                                              InstructionGenerationContext &IGC,
                                              std::optional<uint64_t> MemSeed) {
  if (!MemSeed.has_value())
    return SectionData{SectDesc,
                       getRandomSectionImageForSize(SectDesc.getSize())};
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &M = IGC.getSnippyModule().getModule();
  assert(!M.empty());

  auto &STI = State.getSubtargetImpl(*M.begin());
  const auto &SnpTgt = State.getSnippyTarget();
  auto SectInitialState = SnpTgt.getSectionStateAfterMemInitRoutine(
      ProgCtx, STI, SectDesc.Size, MemSeed.value());
  return SectionData{SectInitialState, SectDesc};
}

SectionData MemInitializationWithAddresses::getSectionData(
    const SectionDesc &SectDesc, InstructionGenerationContext &IGC,
    std::optional<uint64_t> MemSeed) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &MS = IGC.getMemoryAccessSampler();
  auto &SnpTgt = State.getSnippyTarget();
  auto &TM = State.getTargetMachine();
  auto AddrLenInBytes = SnpTgt.getAddrRegLen(TM) / CHAR_BIT;
  auto AccessSize = 1u;
  auto MemMap = MemoryMap{};
  auto Alignment = AccessSize;
  bool TargetIsLittleEndian = TM.getTargetTriple().isLittleEndian();

  if (MemSeed.has_value())
    snippy::warn(WarningName::InconsistentOptions, State.getCtx(),
                 "Memory seed is ignored",
                 "Current mem initialization mode does not use it");

  SmallVector<MemoryUnit, 8> AddrBytes;
  for (size_t SectOffset = 0; SectOffset < SectDesc.Size;
       SectOffset += AddrLenInBytes) {
    auto Access = MS.sample(AddressGenInfo::singleAccess(
        AccessSize, Alignment, /*AllowMisalign=*/false, /*Burst=*/false));
    if (!Access)
      snippy::fatal("Failed to sample memory access for NewCellValue",
                    toString(Access.takeError()));
    auto &NewCellValue = *Access;

    convertNumberToBytesArrayWithEndianness(
        NewCellValue.Address, AddrLenInBytes, TargetIsLittleEndian,
        std::back_inserter(AddrBytes));

    transform(enumerate(AddrBytes), std::inserter(MemMap, MemMap.end()),
              [SectOffset](auto &&Pair) {
                auto &&[Idx, Byte] = Pair;
                return std::pair{SectOffset + Idx, Byte};
              });

    AddrBytes.clear();
  }

  return SectionData{SectDesc, MemMap};
}

ParsedMemoryT ASCIIDumpParser::parse(StringRef FilePath) {
  auto MemBufOrErr = MemoryBuffer::getFile(FilePath);
  if (auto Err = MemBufOrErr.getError())
    snippy::fatal(
        formatv("Can't open file with initial memory: {0}", Err.message()));
  assert(*MemBufOrErr);
  ParsedMemoryT ParsedData;
  FileWrapper Wrapper{std::move(*MemBufOrErr), sys::path::filename(FilePath)};
  while (parseSection(Wrapper, std::back_inserter(ParsedData)))
    ;
  return ParsedData;
}

void MemoryManager::initMemState(planning::InstructionGenerationContext &IGC,
                                 SectionInitFunc InitFunc,
                                 std::optional<uint64_t> MemorySeed) {
  auto RWSectionsRange =
      IGC.getCommonCfg().ProgramCfg.Sections.generalRWSections();
  auto CreateSectState = [&](const SectionDesc &Sect) {
    assert(Sect.M.W() && Sect.M.R() && !Sect.M.X());
    auto SectMemMap = InitFunc(Sect, IGC, MemorySeed);
    checkSectionPresence(MemState, Sect);
    return SectMemMap;
  };
  std::transform(RWSectionsRange.begin(), RWSectionsRange.end(),
                 std::back_inserter(MemState), CreateSectState);
}

void MemoryManager::createMemState(ParsedMemoryT State, GeneratorContext &Ctx) {
  for (auto &[Name, MemMap] : State) {
    auto Desc = getDescription(Name, Ctx);
    if (MemMap.size() != Desc.Size)
      snippy::fatal(formatv("Incorrect section size in memory init file: {0}"
                            " expected {1}, got {2}",
                            Name, Desc.Size, MemMap.size()));
    MemState.emplace_back(Desc, std::move(MemMap));
  }
}

void MemoryManager::fillSectionWithConstants(
    SnippyProgramContext &ProgCtx, Module &M,
    const SectionData &SectData) const {
  constexpr size_t RWSectAlignment = 16;
  auto &State = ProgCtx.getLLVMState();
  if (!isAligned(Align{RWSectAlignment}, SectData.Desc.Size))
    snippy::fatal(formatv("Section {0} can't be emmitted due to misalignment\n",
                          SectData.Desc.getName()));
  MonoAllocatableSection RWSectionManager(SectData.Desc);
  auto ConstantsEmitter =
      GlobalsPool{State, M, RWSectionManager, "", SectData.Desc.getName()};
  // max aligned regions size that may be created with APInt
  constexpr auto RegionSize = 2048ul;
  auto MemorySize = SectData.Desc.Size;
  auto CurRegionOffset = 0ul;
  auto CurRegionEnd = std::min(RegionSize, MemorySize);
  auto MemVector = getContinuosSectMemMap(SectData.MemMap.begin(),
                                          SectData.MemMap.end(), MemorySize);
  while (CurRegionOffset < MemorySize) {
    auto MemoryAPInt = createMemoryRegion(MemVector.begin() + CurRegionOffset,
                                          MemVector.begin() + CurRegionEnd,
                                          SectData.Desc.getName().str());
    createMutableGlobal(ConstantsEmitter, MemoryAPInt, RWSectAlignment);
    CurRegionOffset = CurRegionEnd;
    CurRegionEnd =
        CurRegionOffset + std::min(RegionSize, MemorySize - CurRegionOffset);
  }
}

const Function &
MemoryManager::createMemInitRoutineExtSymbol(Module &M,
                                             LLVMState &State) const {
  return State.createFunction(M, getExternalRandomizerName(),
                              /* SectionName */ "", Function::WeakAnyLinkage);
}

void MemoryManager::materializeSectionDataImpl(
    Module &M, InstructionGenerationContext &IGC, SectionInitFunc InitFunc,
    std::optional<uint64_t> MemorySeed) {
  auto &ProgCtx = IGC.ProgCtx;
  auto RWSectionsRange =
      IGC.getCommonCfg().ProgramCfg.Sections.generalRWSections();
  for (auto &Sect : RWSectionsRange) {
    assert(Sect.M.W() && Sect.M.R() && !Sect.M.X());
    auto SectMemMap = InitFunc(Sect, IGC, MemorySeed);
    checkSectionPresence(MemState, Sect);
    MemState.push_back(SectMemMap);
    fillSectionWithConstants(ProgCtx, M, std::move(SectMemMap));
  }
}

void MemoryManager::createFullRuntimeInit(InstructionGenerationContext &IGC,
                                          MemorySeedTy MemSeed,
                                          bool ExternalCallOfMemInitRoutine) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  const auto &STI = IGC.MBB.getParent()->getSubtarget();
  auto RWSectionsRange =
      IGC.getCommonCfg().ProgramCfg.Sections.generalRWSections();
  auto RP = IGC.pushRegPool();

  SnpTgt.allocateMemoryInitializationRegs(IGC, ExternalCallOfMemInitRoutine);

  if (RWSectionsRange.empty() || ExternalCallOfMemInitRoutine)
    return;

  const auto &ExtRandomSymbol = createMemInitRoutineExtSymbol(
      *IGC.MBB.getParent()->getFunction().getParent(), State);
  for (auto &Sect : RWSectionsRange) {
    assert(Sect.M.W() && Sect.M.R() && !Sect.M.X());
    SnpTgt.generateCallToMemInitRoutine(IGC, Sect.VMA, Sect.Size, MemSeed,
                                        ExtRandomSymbol);
    auto SectMemVector = SnpTgt.getSectionStateAfterMemInitRoutine(
        ProgCtx, STI, Sect.Size, MemSeed);
    MemState.emplace_back(SectMemVector, Sect);
  }
}

void MemoryManager::createLoadsRuntimeInitImpl(
    InstructionGenerationContext &IGC, SectionInitFunc InitFunc,
    bool ExternalCallOfMemInitRoutine, std::optional<uint64_t> MemorySeed) {
  auto &ProgCtx = IGC.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &SnpTgt = State.getSnippyTarget();
  auto RP = IGC.pushRegPool();

  SnpTgt.allocateMemoryInitializationRegs(IGC, ExternalCallOfMemInitRoutine);
  initMemState(IGC, InitFunc, MemorySeed);

  if (ExternalCallOfMemInitRoutine)
    return;

  auto &M = *IGC.MBB.getParent()->getFunction().getParent();
  const auto &ExtInitRoutineSymbol = createMemInitRoutineExtSymbol(M, State);
  auto RA = ProgCtx.getReturnAddress();
  auto *MM = getMetadataMark(State.getCtx(), SnippyMetadata::Support);
  SnpTgt.generateCall(IGC, ExtInitRoutineSymbol, MM, /* Opcode */ std::nullopt,
                      RA);
}

void MemoryManager::writeValueToWriteOnlyAddr(APInt Val, MemAddr Addr) {
  assert(Val.getBitWidth() % CHAR_BIT == 0 && Val.getBitWidth() > 0);
  size_t AccessSize = Val.getBitWidth() / CHAR_BIT;
  auto &Sect = getSection(Addr, AccessSize, MemState.begin(), MemState.end());
  if (!Sect.HasLaterInit)
    snippy::fatal("Attempting to initialize section after initialization");
  assert(Addr >= Sect.Desc.VMA);
  MemAddr LocalAddr = Addr - Sect.Desc.VMA;
  for (size_t CurByteOffset = 0; CurByteOffset < AccessSize; ++CurByteOffset) {
    MemoryUnit CurByte =
        Val.extractBitsAsZExtValue(CHAR_BIT, CurByteOffset * CHAR_BIT);
    [[maybe_unused]] auto Inserted =
        Sect.MemMap.insert({LocalAddr + CurByteOffset, CurByte}).second;
    assert(Inserted && "Byte has already been initializaed");
  }
}

void MemoryManager::addWriteOnlySection(const SectionDesc &Desc) {
  checkSectionPresence(MemState, Desc);
  MemState.emplace_back(Desc, MemoryMap{}, /*HasLaterInit*/ true);
}

void MemoryManager::materializeWriteOnlySections(Module &M,
                                                 GeneratorContext &Ctx) {
  for (auto &WOSectData :
       make_filter_range(MemState, [](const SectionData &SectData) {
         return SectData.HasLaterInit;
       }))
    fillSectionWithConstants(Ctx.getProgramContext(), M, WOSectData);
}

void MemoryManager::markMemAccessToInitialize(const MemAddresses &Addresses,
                                              size_t AccessSize) {
  for (auto Addr : Addresses) {
    auto &Sect = getSection(Addr, AccessSize, MemState.begin(), MemState.end());
    markAccessForSection(Sect, Addr, AccessSize);
  }
}

void MemoryManager::mangleExternalRandomizer(const Linker &L) {
  ExternFuncName = L.getMangledFunctionName(ExternFuncName);
}

} // namespace llvm::snippy
