//===-- Simulator.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Observer.h"
#include "Types.h"

#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <vector>

namespace llvm {
namespace snippy {

class SnippyTarget;

using WarningsT = std::vector<std::string>;

struct IRegisterState {
  virtual ~IRegisterState() {}

  virtual void loadFromYamlFile(StringRef, WarningsT &,
                                const SnippyTarget *Tgt = nullptr) = 0;
  virtual void saveAsYAMLFile(raw_ostream &) const = 0;

  virtual bool operator==(const IRegisterState &) const = 0;
  bool operator!=(const IRegisterState &Another) const {
    return !(*this == Another);
  }

  virtual void randomize() = 0;
};

struct TargetGenContextInterface;

struct SimulationConfig {
  struct Section {
    ProgramCounterType Start = 0;
    ProgramCounterType Size = 0;
    std::string Name;
  };
  std::vector<Section> MemoryRegions;

  std::string TraceLogPath;
};

enum class ExecutionResult {
  Success,
  AttentionRequired,
  SimulationExit,
  FatalError
};

class SimulatorInterface {
public:
  virtual void setStopModeByPC(ProgramCounterType PC) = 0;
  virtual ProgramCounterType readPC() const = 0;
  virtual void setPC(ProgramCounterType PC) = 0;

  virtual RegisterType readGPR(unsigned RegID) const = 0;

  virtual APInt readReg(llvm::Register Reg) const = 0;
  virtual void setReg(llvm::Register Reg, const APInt &NewValue) = 0;

  virtual void setGPR(unsigned RegID, RegisterType NewValue) = 0;

  virtual RegisterType readFPR(unsigned RegID) const = 0;
  virtual void setFPR(unsigned RegID, RegisterType NewValue) = 0;

  virtual VectorRegisterType readVPR(unsigned RegID) const = 0;
  virtual void setVPR(unsigned RegID, const VectorRegisterType &NewValue) = 0;

  virtual void readMem(MemoryAddressType Addr,
                       MutableArrayRef<char> Data) const = 0;
  virtual void writeMem(MemoryAddressType Addr, ArrayRef<char> Data) = 0;

  void writeMem(MemoryAddressType Addr, StringRef Data) {
    writeMem(Addr, ArrayRef<char>{Data.data(), Data.size()});
  }
  void writeMem(MemoryAddressType Addr, const llvm::APInt &Val) {
    writeMem(Addr,
             ArrayRef<char>{reinterpret_cast<const char *>(Val.getRawData()),
                            Val.getBitWidth() / 8});
    ;
  };

  virtual ExecutionResult executeInstr() = 0;

  virtual void saveState(IRegisterState &Regs) const = 0;
  virtual void setState(const IRegisterState &Regs) = 0;

  virtual void logMessage(const Twine &Message) const = 0;

  virtual void dumpSystemRegistersState(raw_ostream &OS) const = 0;

  virtual bool supportsCallbacks() const = 0;

  virtual ~SimulatorInterface() {}
};

} // namespace snippy

template <> struct yaml::MappingTraits<snippy::SimulationConfig> {
  static void mapping(yaml::IO &IO, snippy::SimulationConfig &Cfg);
};

template <> struct yaml::MappingTraits<snippy::SimulationConfig::Section> {
  static void mapping(yaml::IO &IO, snippy::SimulationConfig::Section &S);
};
} // namespace llvm

LLVM_SNIPPY_YAML_IS_SEQUENCE_ELEMENT(llvm::snippy::SimulationConfig::Section,
                                     /* flow */ false);
