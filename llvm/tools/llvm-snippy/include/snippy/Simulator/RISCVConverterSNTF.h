#ifndef LLVM_TOOLS_SNIPPY_RISCV_CONVERTER_SNTF_H
#define LLVM_TOOLS_SNIPPY_RISCV_CONVERTER_SNTF_H

//===-- RISCVConverterSNTF.h---------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/Interpreter.h"
#include "snippy/Simulator/RISCVRegTypes.h"

namespace llvm {
namespace snippy {

struct RegisterLog {
  RegType Type;
  unsigned RegID;
  std::variant<RegisterType, VectorRegisterType> NewValue;
};

struct MemoryLog {
  enum class AccessType { R, W };

  AccessType AccRight;
  MemoryAddressType Addr;
  APInt Value;
};

// This class is a storage of two elements with two methods:
// 1. Read elements.
// 2. Put one at the beginning and drop the last one.
//
// This class is like a window that has two places in it and it can shift:
// clang-format off
//                     _______________
// ___________________|_______________|___________________
//|      | ---> |     || ---> |      ||---> |      | ---> |
//|______|______|_____||______|______||_____|______|______|
//                    |_______________|
//
// clang-format on
template <typename T> class DualWindow {
  std::pair<T, T> Elems;

public:
  const T &first() const { return Elems.first; }
  const T &second() const { return Elems.second; }
  void shift(const T &NewElem) {
    Elems.second = NewElem;
    std::swap(Elems.first, Elems.second);
  }
  void setFirst(const T &First) { Elems.first = First; }
};

// RISCVConverterSNTF converts the model trace into Snippy Trace Format (SNTF)
// and prints it in provided file.
//
// Format:
//
// SNTF file begins with a header, in which after # (comments), starting from a
// new line, are followed:
//   1. Format name (SNTF)
//   2. Version
//   3. List of enabled features of the current version +<feature-name>
//      (ex: +time)
//   4. List of disabled features of the current version -<feature-name>
//      (ex: -next-pc)
//
// In total, paragraphs 3 and 4 should list all the features of the version from
// paragraph 2.
//
// All names of 1.0 version features:
//   * time
//   * pc
//   * instr-code
//   * next-pc
//   * registers-changed
//   * memory-accesses
//
// Header example:
//   # SNTF
//   # 1.0
//   # +time
//   # +pc
//   # +instr-code
//   # +next-pc
//   # +registers-changed
//   # +memory-accesses
//
// After the header, on each line placed entries that were selected in the
// header, separated by a whitespace.
//
// Entry format:
//
// clang-format off
// _____________________________________________________________________________
//|Time in|Program |Instruction|Next    |Register   |Memory                     |
//|nanosec|counter |code       |program |values if  |addrs if                   |
//|       |        |           |counter |changed    |changed or read            |
//|_______|________|___________|________|___________|___________________________|
//|520    |000086d0|11010093   |000086d4|f4=017f7ff0|2ff4fe98:W:fa,2ff4fe99:R:ee|
//|_______|________|___________|________|___________|___________________________|
// * Time is synchronized with when the instruction was fetched from memory. For
//   now it it just a number of instruction.
// * Vector registers are printed element by element, the length of which is 64 bits:
//   (vlen=128) v4=[1]:0000000000227c3d[0]:0000000100227c3d
//   (vlen=256) v7=[3]:00000f0000000001[2]:0000000000000a00[1]:0000b00000000000[0]:00000c0000000000
// * The format of memory accesses: <Addr>:<AccessType>:<Value>. Access size is
//   determined by the number of characters in the Value. AccessType can be
//   either W (write) or R (read).
//
// Entry example:
//   520 000086d0 11010093 000086d4 f4=017f7ff0 2ff4fe98:W:fa,2ff4fe99:R:ee
//
// clang-format on

#define SNTF_VERSION "1.0"

struct FeatureFlagsSNTF {
  bool EnableTime = true;
  bool EnablePC = true;
  bool EnableInstrCode = true;
  bool EnableNextPC = true;
  bool EnableRegVals = true;
  bool EnableMemAccesses = true;

  bool operator==(const FeatureFlagsSNTF &Other) const {
    return std::tie(EnableTime,
                    EnablePC, EnableInstrCode, EnableNextPC, EnableRegVals,
                    EnableMemAccesses) ==
           std::tie(Other.EnableTime,
                    Other.EnablePC, Other.EnableInstrCode, Other.EnableNextPC,
                    Other.EnableRegVals, Other.EnableMemAccesses);
  }
};

class RISCVConverterSNTF {
  // Vector register can be printed element by element, where the length of one
  // element is 64 bits.
  static constexpr auto VecElemWidth = 64;

  unsigned long long Time = 0;
  const unsigned XLen;
  std::optional<unsigned> FLen;
  const Interpreter &SimInterpreter;
  const ProgramCounterType LastPC;
  std::unique_ptr<raw_fd_ostream> TraceFile;
  // We use a DualWindow to keep track of the last two PCs values
  // and shift at each PC update.
  DualWindow<std::optional<ProgramCounterType>> PrevPCs;
  using FeatureFuncT = std::function<void(raw_ostream &)>;
  std::vector<FeatureFuncT> Features;
  ArrayRef<RegisterLog> RegLogs;
  ArrayRef<MemoryLog> MemLogs;

  static std::string getVectorDataSNTF(const VectorRegisterType &VecData);
  void printRegLog(raw_ostream &LogsBuff, const RegisterLog &RegLog) const;
  void printMemLog(raw_ostream &LogsBuff, const MemoryLog &MemLog) const;
  void logHeader(raw_ostream &LogsBuff) const;

public:
  RISCVConverterSNTF(const unsigned XLen,
                     const ProgramCounterType StartPC,
                     const ProgramCounterType LastPC, const Interpreter &I,
                     StringRef Path, FeatureFlagsSNTF FeatureFlags = {});

  void acceptRecordsAndShiftPC(ProgramCounterType PC,
                               ArrayRef<RegisterLog> RegLogs,
                               [[maybe_unused]] ArrayRef<MemoryLog> MemLogs);
  void acceptRecords(raw_ostream &LogsBuff, ProgramCounterType PC,
                     ArrayRef<RegisterLog> RegLogs,
                     ArrayRef<MemoryLog> MemLogs);
};

} // namespace snippy
} // namespace llvm

#endif // LLVM_TOOLS_SNIPPY_RISCV_CONVERTER_SNTF_H
