//===-- Interpreter.cpp ------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Class to execute binary code on a simulator and inspect the result.
///
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Interpreter.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Linker.h"

#include "snippy/Generator/MemoryManager.h"
#include "snippy/Support/DynLibLoader.h"
#include "snippy/Support/Options.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>

#define DEBUG_TYPE "snippy-interpreter"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string> TraceLogPath(
    "trace-log",
    cl::desc("execution log file. Execution logs are written to the standard "
             "output if not specified"),
    cl::cat(Options), cl::init(""));

static snippy::opt<bool>
    DumpAsASCII("dump-memory-as-ascii",
                cl::desc("Memory dump will be in ASCII format"),
                cl::init(false));

} // namespace snippy
} // namespace llvm

#define ARCH_PREFIX "riscv"

namespace llvm {

static std::string makeModelNameFromPartialName(StringRef PartialName) {
  return (Twine(ARCH_PREFIX) + "-" + PartialName + "-plugin.so").str();
}

static std::unique_ptr<object::ObjectFile> makeObjectFile(MemoryBufferRef Buf) {
  auto Exp = object::ObjectFile::createObjectFile(Buf);
  if (!Exp)
    report_fatal_error("Failed to constuct object file from memory buffer");
  return std::move(Exp.get());
}

static auto getSectionIt(object::ObjectFile &Obj, StringRef SectionName) {
  auto SecIt = std::find_if(Obj.section_begin(), Obj.section_end(),
                            [&SectionName](const auto &S) {
                              auto Exp = S.getName();
                              if (!Exp)
                                return false;
                              return Exp.get() == SectionName;
                            });
  return SecIt;
}

static bool hasSection(object::ObjectFile &Obj, StringRef SectionName) {
  return getSectionIt(Obj, SectionName) != Obj.section_end();
}

static StringRef getSectionData(object::ObjectFile &Obj,
                                StringRef SectionName) {
  auto SectionIt = getSectionIt(Obj, SectionName);
  assert(SectionIt != Obj.section_end());

  auto Exp = SectionIt->getContents();
  if (!Exp)
    report_fatal_error(SectionName + " section is empty");
  return Exp.get();
}

static auto getSectionLA(object::ObjectFile &Obj, StringRef SectionName) {
  auto SectionIt = getSectionIt(Obj, SectionName);
  assert(SectionIt != Obj.section_end());
  return SectionIt->getAddress();
}

} // namespace llvm

namespace llvm {
namespace snippy {

namespace {
void applyMemCfgToSimCfg(const MemoryConfig &MemCfg, SimulationConfig &SimCfg) {
  llvm::transform(MemCfg.ProgSections, std::back_inserter(SimCfg.ProgSections),
                  [](auto &Section) {
                    return SimulationConfig::Section{
                        Section.Start, Section.Size, Section.Name};
                  });
  SimCfg.RomStart = MemCfg.Rom.Start;
  SimCfg.RomSize = MemCfg.Rom.Size;
  SimCfg.RomSectionName = MemCfg.Rom.Name;
  SimCfg.RamStart = MemCfg.Ram.Start;
  SimCfg.RamSize = MemCfg.Ram.Size;
}
} // namespace

std::unique_ptr<SimulatorInterface> Interpreter::createSimulatorForTarget(
    const SnippyTarget &TGT, const TargetSubtargetInfo &Subtarget,
    const SimulationConfig &SimCfg, const TargetGenContextInterface *TgtGenCtx,
    RVMCallbackHandler *CallbackHandler, std::string ModelLibrary) {
  auto Lib =
      DynamicLibrary(std::move(ModelLibrary), makeModelNameFromPartialName);
  auto Sim =
      TGT.createSimulator(Lib, SimCfg, TgtGenCtx, CallbackHandler, Subtarget);
  if (!Sim)
    llvm::report_fatal_error("could not initialize simulator", false);
  return Sim;
}

template <typename It>
static void writeAsHex(It Beg, It End, raw_string_ostream &SS) {
  for (auto Symbol : make_range(Beg, End))
    SS << ' '
       << format_hex_no_prefix(static_cast<unsigned char>(Symbol), /*Width*/ 2);
}

static void writeSectionToFile(ArrayRef<char> Data,
                               const NamedMemoryRange &Range,
                               raw_fd_ostream &File) {
  auto *SectData = Data.data();
  if (!DumpAsASCII) {
    File.write(SectData, Data.size());
    return;
  }

  auto SectionHeader = Range.hasName() ? "Section {" + Range.name() + "}:"
                                       : "Range {" + Range.name() + "}:";
  std::string StringForSection;
  raw_string_ostream SS{StringForSection};
  SS << SectionHeader << "\n";
  // Number of digits of max address value in section in hex
  auto NumOfDigits = (Data.size() > 1)
                         ? static_cast<size_t>(std::ceil(
                               std::log10(Data.size() - 1) / std::log10(16)))
                         : 0ul;
  auto CurOffset = 0ul;
  auto LineSize = 0x10ul;
  while (CurOffset < Data.size()) {
    // This additional 2 is a ritual sacrifice for the developer of the
    // format_hex(), who have decided to consider 0x as a part of a NUMBER
    SS << format_hex(CurOffset, NumOfDigits + 2) << ": ";
    auto CurLineSize = std::min(LineSize, Data.size() - CurOffset);
    auto CurLineBeg = SectData + CurOffset;
    writeAsHex(CurLineBeg, CurLineBeg + CurLineSize, SS);
    SS << "\n";
    CurOffset += CurLineSize;
  }
  File << SS.str();
}

SimulationEnvironment Interpreter::createSimulationEnvironment(
    const SnippyTarget &SnippyTGT, const TargetSubtargetInfo &ST,
    const Linker &L, const MemoryScheme &MS,
    const TargetGenContextInterface &TgtCtx, bool NeedCallbackHandler) {

  SimulationEnvironment Env;
  Env.SnippyTGT = &SnippyTGT;
  Env.ST = &ST;

  auto MemCfg = MemoryConfig::getMemoryConfig(L);
  applyMemCfgToSimCfg(MemCfg, Env.SimCfg);

  std::transform(L.sections().begin(), L.sections().end(),
                 std::back_inserter(Env.Sections),
                 [](auto &SE) { return SE.OutputSection.Desc; });

  Env.SimCfg.TraceLogPath = TraceLogPath.getValue();
  Env.TgtGenCtx = &TgtCtx;

  if (NeedCallbackHandler)
    Env.CallbackHandler = std::make_unique<RVMCallbackHandler>();

  return Env;
}

Interpreter::Interpreter(LLVMContext &Ctx, const SimulationEnvironment &SimEnv,
                         std::unique_ptr<SimulatorInterface> Sim)
    : Simulator(std::move(Sim)), Env(SimEnv) {
  Simulator->setPC(getProgStart());
  if (auto *Handler = Env.CallbackHandler.get())
    TransactionsObserverHandle =
        Handler->createAndSetObserver<TransactionStack>();
}

Interpreter::Interpreter(LLVMContext &Ctx, const SimulationEnvironment &Env,
                         std::unique_ptr<SimulatorInterface> Sim,
                         const IRegisterState &Regs)
    : Interpreter(Ctx, Env, std::move(Sim)) {
  Simulator->setState(Regs);
}

bool Interpreter::compareStates(const Interpreter &Another,
                                bool CheckMemory) const {
  auto RS1 = Env.SnippyTGT->createRegisterState(*Env.ST);
  auto RS2 = Env.SnippyTGT->createRegisterState(*Env.ST);
  Simulator->saveState(*RS1);
  Another.Simulator->saveState(*RS2);
  if (*RS1 != *RS2)
    return false;
  if (!CheckMemory)
    return true;

  std::vector<char> MI1, MI2;

  MI1.resize(getRamSize());
  MI2.resize(getRamSize());

  Simulator->readMem(getRamStart(), MI1);
  Another.Simulator->readMem(getRamStart(), MI2);
  return MI1 == MI2;
}

bool Interpreter::step() {
  return (Simulator->executeInstr() == ExecutionResult::Success);
}

bool Interpreter::endOfProg() const {
  return Simulator->readPC() == getProgEnd();
}

void Interpreter::resetMem() {
  const auto &SimCfg = Env.SimCfg;
  std::vector<char> RAMZeroMem(SimCfg.RamSize, 0);
  std::vector<char> ROMZeroMem(SimCfg.RomSize, 0);

  Simulator->writeMem(SimCfg.RamStart, RAMZeroMem);
  Simulator->writeMem(SimCfg.RomStart, ROMZeroMem);
  for (auto &Section : SimCfg.ProgSections) {
    std::vector<char> ProgZeroMem(Section.Size, 0);
    Simulator->writeMem(Section.Start, ProgZeroMem);
  }
}

void Interpreter::disableTransactionsTracking() {
  if (TransactionsObserverHandle)
    Env.CallbackHandler->eraseByHandle(*TransactionsObserverHandle);
  TransactionsObserverHandle.reset();
}

void Interpreter::dumpCurrentRegState(StringRef Filename) const {
  auto RegisterState = Env.SnippyTGT->createRegisterState(*Env.ST);
  Simulator->saveState(*RegisterState);
  dumpRegs(*RegisterState, Filename);
}

void Interpreter::loadElfImage(StringRef ElfImage) {
  std::string ProgramText;
  auto MemBuff = MemoryBuffer::getMemBuffer(ElfImage, "", false);
  auto ObjectFile = makeObjectFile(*MemBuff);

  auto EndOfProgSym = llvm::find_if(ObjectFile->symbols(), [](auto &Sym) {
    auto EName = Sym.getName();
    return EName && EName.get() == Linker::GetExitSymbolName();
  });

  assert(EndOfProgSym != ObjectFile->symbols().end() &&
         "expected to have one of these");
  auto EAddress = EndOfProgSym->getAddress();
  assert(EAddress && "Expected the address of symbol to be known");
  ProgEnd = EAddress.get();

  for (auto &ProgSection : Env.SimCfg.ProgSections) {
    assert(hasSection(*ObjectFile, ProgSection.Name));

    ProgramText = getSectionData(*ObjectFile, ProgSection.Name);

    if (ProgSection.Size < ProgramText.size())
      report_fatal_error(
          "RX section '" + Twine(ProgSection.Name) +
              "' failed to fit code mapped to it: section size is " +
              Twine(ProgSection.Size) + " and code size is " +
              Twine(ProgramText.size()),
          false);
    Simulator->writeMem(ProgSection.Start, ProgramText);
  }
  if (!Env.SimCfg.RomSectionName.empty()) {
    auto RODataName = Env.SimCfg.RomSectionName;
    // Elf image might not have this section if
    // .rodata happens to be empty.
    if (hasSection(*ObjectFile, RODataName)) {
      auto SnippyData = getSectionData(*ObjectFile, RODataName);
      Simulator->writeMem(getSectionLA(*ObjectFile, RODataName), SnippyData);
    }
  }

  // adding sections with non-zero data
  for (auto &It : Env.SimCfg.AdditionalSectionsNames) {
    auto Data = getSectionData(*ObjectFile, It);
    Simulator->writeMem(getSectionLA(*ObjectFile, It), Data);
  }
}

void Interpreter::addInstr(const MachineInstr &MI, const LLVMState &State) {
  SmallVector<char> EncodedMI;
  const auto &SnippyTgt = State.getSnippyTarget();

  SnippyTgt.getEncodedMCInstr(&MI, State.getCodeEmitter(),
                              State.getOrCreateAsmPrinter(),
                              State.getSubtargetInfo(), EncodedMI);
  Simulator->writeMem(Simulator->readPC(), EncodedMI);
}

void Interpreter::dumpRegsAsYAML(const IRegisterState &Regs,
                                 StringRef FileName) {
  Regs.saveAsYAMLFile(FileName);
}

void Interpreter::dumpRegs(const IRegisterState &Regs, StringRef YamlPath) {
  if (!YamlPath.empty())
    dumpRegsAsYAML(Regs, YamlPath);
}

void Interpreter::dumpOneRange(NamedMemoryRange Range,
                               raw_fd_ostream &File) const {
  assert(Range.isValid());
  auto [RangeBeg, RangeEnd] = Range.boundaries();
  auto Size = RangeEnd - RangeBeg;
  std::vector<char> Data(Size);
  Simulator->readMem(RangeBeg, Data);

  writeSectionToFile(Data, Range, File);
  if (File.has_error())
    report_fatal_error("Memory dump error: " + Twine(File.error().message()),
                       false);
}

std::optional<NamedMemoryRange>
Interpreter::getSectionPosition(StringRef SectionName) const {
  const auto &Sections = Env.Sections;
  auto S = std::find_if(Sections.begin(), Sections.end(),
                        [SectionName](const auto &CurSection) {
                          return CurSection.getIDString() == SectionName;
                        });
  if (S == Sections.end())
    return std::nullopt;
  return {NamedMemoryRange{S->VMA, S->VMA + S->Size, S->getIDString()}};
}

void Interpreter::dumpRanges(ArrayRef<NamedMemoryRange> Ranges,
                             const std::string &FileName) const {
  std::error_code EC;
  raw_fd_ostream File(FileName, EC);
  if (EC)
    report_fatal_error("Memory dump error: " + Twine(EC.message()), false);

  for (auto Range : Ranges)
    dumpOneRange(Range, File);
}

void Interpreter::setReg(llvm::Register Reg, const APInt &NewValue) {
  Simulator->setReg(Reg, NewValue);
  auto RegIdx = Env.SnippyTGT->regToIndex(Reg);
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);
  switch (Env.SnippyTGT->regToStorage(Reg)) {
  case RegStorageType::XReg:
    Transactions.xregUpdateNotification(RegIdx, NewValue.getZExtValue());
    return;
  case RegStorageType::FReg:
    Transactions.fregUpdateNotification(RegIdx, NewValue.getZExtValue());
    return;
  case RegStorageType::VReg:
    Transactions.vregUpdateNotification(
        RegIdx, {reinterpret_cast<const char *>(NewValue.getRawData()),
                 NewValue.getBitWidth() / CHAR_BIT});
    return;
  }
  llvm_unreachable("unknown storage");
}

void Interpreter::initTransactionMechanism() {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);
  assert(Transactions.empty());

  SmallVector<std::pair<uint64_t, uint64_t>, 3> MemoryConfig = {
      std::pair{getRomStart(), getRomSize()},
      std::pair{getRamStart(), getRamSize()}};
  for (auto &Section : Env.SimCfg.ProgSections)
    MemoryConfig.emplace_back(std::make_pair(Section.Start, Section.Size));
  for (auto [Start, Size] : MemoryConfig) {
    if (Size == 0)
      continue;
    std::vector<char> Snapshot(Size);
    Simulator->readMem(Start, Snapshot);
    Transactions.addMemSnapshot(Start, std::move(Snapshot));
  }

  Transactions.addPCToSnapshot(Simulator->readPC());

  for (auto i = 0u;
       i < Env.SnippyTGT->getNumRegs(RegStorageType::XReg, *Env.ST); ++i) {
    auto Value = Simulator->readGPR(i);
    Transactions.addXRegToSnapshot(i, Value);
  }
  for (auto i = 0u;
       i < Env.SnippyTGT->getNumRegs(RegStorageType::FReg, *Env.ST); ++i) {
    auto Value = Simulator->readFPR(i);
    Transactions.addFRegToSnapshot(i, Value);
  }
  for (auto i = 0u;
       i < Env.SnippyTGT->getNumRegs(RegStorageType::VReg, *Env.ST); ++i) {
    auto Value = Simulator->readVPR(i);
    Transactions.addVRegToSnapshot(i, std::move(Value));
  }
}

void Interpreter::openTransaction() {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  if (Transactions.empty())
    initTransactionMechanism();

  Transactions.push();
}

void Interpreter::commitTransaction() {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());

  // When a state is committed, we must propagate the current state change to
  // the open transaction (that is previous in active transactions stack).
  // Example:
  //     x = 1
  //   --- add state       , snapshot: x = 1
  //     x = 2             , snapshot: x = 1, cur state x = 2
  //   --- add state       , snapshot: x = 1, prev state x = 2, cur state is
  //                         empty
  //     x = 3             , snapshot: x = 1, prev state x = 2, cur state x = 3
  //   --- commit state    , snapshot: x = 1, cur state x = 3 (! not 2)
  //   --- discard state   , snapshot: x = 1, cur state is empty, write x = 1 in
  //                         simulator
  //   --- commit state    , empty.
  TransactionStack::AddrToDataType CurMemChange;
  TransactionStack::RegIdToValueType CurXRegsChange;
  TransactionStack::RegIdToValueType CurFRegsChange;
  TransactionStack::VRegIdToValueType CurVRegsChange;
  auto CurPCChange = Transactions.getPC();
  if (Transactions.size() > 1) {
    CurMemChange = Transactions.getMemChangedByTransaction();
    CurXRegsChange = Transactions.getXRegsChangedByTransaction();
    CurFRegsChange = Transactions.getFRegsChangedByTransaction();
    CurVRegsChange = Transactions.getVRegsChangedByTransaction();
  }
  Transactions.pop();

  Transactions.PCUpdateNotification(CurPCChange);
  for (auto [Addr, Data] : CurMemChange)
    Transactions.memUpdateNotification(Addr, &Data, sizeof(Data));
  for (const auto &[RegId, Value] : CurXRegsChange)
    Transactions.xregUpdateNotification(RegId, Value);
  for (const auto &[RegId, Value] : CurFRegsChange)
    Transactions.fregUpdateNotification(RegId, Value);
  for (const auto &[RegId, Value] : CurVRegsChange) {
    assert(Value.getBitWidth() % CHAR_BIT == 0);
    Transactions.vregUpdateNotification(
        RegId, {reinterpret_cast<const char *>(Value.getRawData()),
                Value.getBitWidth() / CHAR_BIT});
  }
}

void Interpreter::discardTransaction() {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());

  Simulator->setPC(getPCBeforeTransaction());
  for (auto [Addr, Value] : getMemBeforeTransaction())
    Simulator->writeMem(Addr, APInt(sizeof(Value) * CHAR_BIT, Value));
  for (auto [RegID, Value] : getXRegsBeforeTransaction())
    Simulator->setGPR(RegID, Value);
  for (auto [RegID, Value] : getFRegsBeforeTransaction())
    Simulator->setFPR(RegID, Value);
  for (auto [RegID, Value] : getVRegsBeforeTransaction())
    Simulator->setVPR(RegID, Value);

  Transactions.clearLastTransaction();
}

TransactionStack::AddrToDataType Interpreter::getMemBeforeTransaction() const {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());
  return Transactions.getMemBeforeTransaction();
}

TransactionStack::RegIdToValueType
Interpreter::getXRegsBeforeTransaction() const {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());
  return Transactions.getXRegsBeforeTransaction();
}

TransactionStack::RegIdToValueType
Interpreter::getFRegsBeforeTransaction() const {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());
  return Transactions.getFRegsBeforeTransaction();
}

TransactionStack::VRegIdToValueType
Interpreter::getVRegsBeforeTransaction() const {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());
  return Transactions.getVRegsBeforeTransaction();
}

ProgramCounterType Interpreter::getPCBeforeTransaction() const {
  auto &Transactions =
      Env.CallbackHandler->getObserverByHandle(*TransactionsObserverHandle);

  assert(!Transactions.empty());
  return Transactions.getPCBeforeTransaction();
}

} // namespace snippy
} // namespace llvm
