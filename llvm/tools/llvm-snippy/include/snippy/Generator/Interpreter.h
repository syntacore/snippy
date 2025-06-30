#ifndef LLVM_TOOLS_SNIPPY_LIB_INTERPRETER_H
#define LLVM_TOOLS_SNIPPY_LIB_INTERPRETER_H

//===-- Interpreter.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Class to execute binary code on SAIL and inspect the result.
///
//===----------------------------------------------------------------------===//

#include "LLVMState.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include "snippy/Generator/ParsedElf.h"
#include "snippy/Simulator/Simulator.h"
#include "snippy/Simulator/Transactions.h"

#include <algorithm>
#include <vector>

namespace llvm {
namespace snippy {

class MemoryScheme;
class GeneratorContext;
struct TargetGenContextInterface;
struct SectionData;
class MemoryAccessSampler;
class SnippyProgramContext;
class Config;

using SectionDescVect = std::vector<SectionDesc>;
struct SimulationEnvironment {
  const SnippyTarget *SnippyTGT;
  const TargetSubtargetInfo *ST;
  SimulationConfig SimCfg;
  const TargetGenContextInterface *TgtGenCtx = nullptr;
  std::unique_ptr<RVMCallbackHandler> CallbackHandler;
  SectionDescVect Sections;
};

class NamedMemoryRange {
  MemAddr Beg = 0;
  MemAddr End = 0;
  std::optional<std::string> Name;

public:
  NamedMemoryRange(MemAddr Beg, MemAddr End,
                   std::optional<std::string> Name = std::nullopt)
      : Beg{Beg}, End{End}, Name{Name} {}

  std::pair<MemAddr, MemAddr> boundaries() const { return {Beg, End}; }

  bool isValid() const { return End > Beg; }

  std::string name() const {
    if (Name)
      return *Name;

    std::string HeaderString;
    raw_string_ostream SS{HeaderString};
    SS << "0x";
    SS.write_hex(Beg);
    SS << "-0x";
    SS.write_hex(End);
    return SS.str();
  }

  bool hasName() const { return Name.has_value(); }

  bool operator<(const NamedMemoryRange &Rhs) const {
    return std::tie(Beg, End) < std::tie(Rhs.Beg, Rhs.End);
  }

  auto operator==(const NamedMemoryRange &Rhs) const {
    return !(*this < Rhs) && !(Rhs < *this);
  }
};

class Interpreter final {
  std::unique_ptr<SimulatorInterface> Simulator;
  const SimulationEnvironment &Env;
  std::unique_ptr<RVMCallbackHandler::ObserverHandle<TransactionStack>>
      TransactionsObserverHandle;
  void initTransactionMechanism();
  void dumpOneRange(NamedMemoryRange Range, raw_fd_ostream &OS) const;
  bool coveredByMemoryRegion(MemAddr Start, MemAddr Size) const;

  static StringRef getStringNameOrUnknown(Expected<StringRef> Str) {
    static constexpr StringRef UnknownData = "<<UNKNOWN>>";
    return Str.takeError() ? UnknownData : *Str;
  };

  template <typename SectionRangeT>
  Error checkSectionsCoveredByMemoryRegion(SectionRangeT &&Sections) const {
    auto NotCovered = llvm::find_if(Sections, [this](auto &&Section) {
      auto Address = Section.getAddress();
      auto Size = Section.getSize();

      return !coveredByMemoryRegion(Address, Size);
    });

    if (NotCovered == Sections.end())
      return Error::success();

    auto SectionName = NotCovered->getName();

    auto Address = NotCovered->getAddress();

    auto Size = NotCovered->getSize();

    auto Err = createStringError(
        makeErrorCode(Errc::CorruptedElfImage),
        formatv("Trying to load/allocate section '{0}' at address 0x{1:x} of "
                "size 0x{2:x} which is not covered by model memory region",
                getStringNameOrUnknown(std::move(SectionName)), Address, Size));

    return joinErrors(std::move(Err), SectionName.takeError());
  }

public:
  static SimulationEnvironment createSimulationEnvironment(
      SnippyProgramContext &SPC, const TargetSubtargetInfo &ST,
      const Config &Settings, TargetGenContextInterface &TgtCtx);

  static std::unique_ptr<SimulatorInterface> createSimulatorForTarget(
      const SnippyTarget &TGT, const TargetSubtargetInfo &Subtarget,
      const SimulationConfig &SimCFG,
      const TargetGenContextInterface *TgtGenCtx,
      RVMCallbackHandler *CallbackHandler, std::string ModelLibrary);

  Interpreter(LLVMContext &Ctx, const SimulationEnvironment &Env,
              std::unique_ptr<SimulatorInterface> Sim);

  Interpreter(LLVMContext &Ctx, const SimulationEnvironment &Env,
              std::unique_ptr<SimulatorInterface> Sim,
              const IRegisterState &Regs);

  bool compareStates(const Interpreter &Another,
                     bool CheckMemory = false) const;

  [[nodiscard]] ExecutionResult step() { return Simulator->executeInstr(); }

  void resetState(const SnippyProgramContext &ProgCtx, bool DoMemReset = true);
  void resetMem();

  void disableTransactionsTracking();
  using SectionFilterPFN = std::function<bool(llvm::object::SectionRef)>;
  // Loads elf image into simulator and sets PC to
  // entry point address. Passing empty string to EntryPointSymbol searches for
  // default entry point defined in elf.
  // InitBSS controls whether bss sections should be zeroed out.
  Error loadElfImage(const ParsedElf &ElfData, bool InitBSS) {
    if (auto Err =
            checkSectionsCoveredByMemoryRegion(ElfData.TextAndDataSections))
      return Err;

    if (auto Err = checkSectionsCoveredByMemoryRegion(ElfData.BSSSections))
      return Err;

    for (auto &&Section : ElfData.TextAndDataSections) {
      if (auto EContents = Section.getContents()) {
        snippy::warn(WarningName::EmptyElfSection,
                     formatv("ignored LOAD section '{0}'",
                             getStringNameOrUnknown(Section.getName())),
                     "empty contents");
        auto Address = Section.getAddress();
        Simulator->writeMem(Address, *EContents);
      }
    }

    if (!InitBSS)
      return Error::success();

    for (auto &&Section : ElfData.BSSSections) {
      auto Size = Section.getSize();
      auto Address = Section.getAddress();
      std::vector<char> Zeroes(Size, 0);
      Simulator->writeMem(Address, Zeroes);
    }

    return Error::success();
  }

  void dumpCurrentRegState(StringRef Filename) const;

  void dumpCurrentRegStateToStream(raw_ostream &OS) const;

  void dumpSystemRegistersState(raw_ostream &OS) const;

  [[noreturn]] void reportSimulationFatalError(StringRef PrefixMessage) const;

  void setRegisterState(const IRegisterState &Regs) {
    Simulator->setState(Regs);
  }

  void setStopModeByPC(ProgramCounterType StopPC) {
    Simulator->setStopModeByPC(StopPC);
  }

  template <typename InstrIt>
  [[nodiscard]] bool executeChainOfInstrs(const LLVMState &State,
                                          InstrIt ItBegin, InstrIt ItEnd) {
    for (auto ItCur = ItBegin; ItCur != ItEnd; ++ItCur) {
      addInstr(*ItCur, State);
      if (step() != ExecutionResult::Success)
        return false;
    }
    return true;
  }

  template <typename ObserverType, typename... CtorArgs>
  std::unique_ptr<RVMCallbackHandler::ObserverHandle<ObserverType>>
  setObserver(CtorArgs &&...Args) {
    return Env.CallbackHandler->createAndSetObserver<ObserverType>(
        std::forward<CtorArgs>(Args)...);
  }

  template <typename ObserverHandleType>
  auto &getObserverByHandle(const ObserverHandleType &Handle) {
    return Env.CallbackHandler->getObserverByHandle(Handle);
  }

  void addInstr(const MachineInstr &MI, const LLVMState &State);

  ProgramCounterType getPC() const { return Simulator->readPC(); }

  void setPC(ProgramCounterType PC) { Simulator->setPC(PC); }

  bool modelSupportCallbacks() const { return Simulator->supportsCallbacks(); }

  APInt readReg(llvm::Register Reg) const { return Simulator->readReg(Reg); };
  void setReg(llvm::Register Reg, const APInt &NewValue);

  void readMem(MemoryAddressType Addr, MutableArrayRef<char> Data) const {
    Simulator->readMem(Addr, Data);
  }
  void writeMem(MemoryAddressType Addr, ArrayRef<char> Data) {
    Simulator->writeMem(Addr, Data);
  };
  void writeMem(MemoryAddressType Addr, StringRef Data) {
    Simulator->writeMem(Addr, Data);
  }
  void writeMem(MemoryAddressType Addr, const llvm::APInt &Val) {
    Simulator->writeMem(Addr, Val);
  }

  const auto &getSubTarget() const { return *Env.ST; }

  const auto &getSections() const { return Env.Sections; }

  const auto &getSimCfg() const { return Env.SimCfg; }

  std::optional<NamedMemoryRange> getSectionPosition(StringRef Name) const;

  static void dumpRegsAsYAML(const IRegisterState &Regs, raw_ostream &OS);
  static void dumpRegs(const IRegisterState &Regs, StringRef YamlPath);
  void dumpRanges(ArrayRef<NamedMemoryRange> SectionNames,
                  const std::string &FileName) const;

  void logMessage(const Twine &Message) const {
    return Simulator->logMessage(Message);
  }

  void openTransaction();
  void commitTransaction();
  void discardTransaction();
  // get original values of the memory/registers changed by the last open
  // transaction.
  TransactionStack::AddrToDataType getMemBeforeTransaction() const;
  TransactionStack::RegIdToValueType getXRegsBeforeTransaction() const;
  TransactionStack::RegIdToValueType getFRegsBeforeTransaction() const;
  TransactionStack::VRegIdToValueType getVRegsBeforeTransaction() const;
  ProgramCounterType getPCBeforeTransaction() const;
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_LIB_INTERPRETER_H
