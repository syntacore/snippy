//===-- RISCVConverterSNTF.cpp-------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/RISCVConverterSNTF.h"
#include "../lib/Target/RISCV/RISCVGenerated.h"

namespace llvm {
namespace snippy {


RISCVConverterSNTF::RISCVConverterSNTF(const unsigned XL,
                                       const ProgramCounterType StartPC,
                                       const ProgramCounterType LastPC,
                                       const Interpreter &I, StringRef Path,
                                       FeatureFlagsSNTF FeatureFlags)
    : XLen(XL),
      FLen(getRISCVFloatRegLen(I.getSubTarget())), SimInterpreter(I),
      LastPC(LastPC) {
  std::error_code EC;
  TraceFile = std::make_unique<raw_fd_ostream>(Path, EC);
  if (EC)
    snippy::fatal(formatv("Can't open file for trace printing: {0}: {1}",
                          EC.message(), Path));
  PrevPCs.setFirst(StartPC);

  if (FeatureFlags.EnableTime)
    Features.push_back([this](raw_ostream &LogsBuff) {
      assert(Time >= 2);
      LogsBuff << Time - 2 << " ";
    });
  if (FeatureFlags.EnablePC)
    Features.push_back([this](raw_ostream &LogsBuff) {
      LogsBuff << toHexStringTruncate(PrevPCs.second().value(), XLen) << " ";
    });
  if (FeatureFlags.EnableInstrCode)
    Features.push_back([this](raw_ostream &LogsBuff) {
      assert(PrevPCs.second().has_value() && "PC must be legal");
      MaxInstrBitsType InstrCode =
          readInstrCode(SimInterpreter, PrevPCs.second().value());
      LogsBuff << format_hex_no_prefix(InstrCode, sizeof(MaxInstrBitsType) * 2)
               << " ";
    });
  if (FeatureFlags.EnableNextPC)
    Features.push_back([this](raw_ostream &LogsBuff) {
      LogsBuff << toHexStringTruncate(PrevPCs.first().value(), XLen);
    });
  if (FeatureFlags.EnableRegVals)
    Features.push_back([this](raw_ostream &LogsBuff) {
      if (!RegLogs.empty()) {
        LogsBuff << " ";
        printRegLog(LogsBuff, *RegLogs.begin());
        llvm::for_each(llvm::drop_begin(RegLogs), [&](const auto &RegLog) {
          LogsBuff << ",";
          printRegLog(LogsBuff, RegLog);
        });
      }
    });
  if (FeatureFlags.EnableMemAccesses)
    Features.push_back([this](raw_ostream &LogsBuff) {
      if (!MemLogs.empty()) {
        LogsBuff << " ";
        printMemLog(LogsBuff, *MemLogs.begin());
        llvm::for_each(llvm::drop_begin(MemLogs), [&](const auto &MemLog) {
          LogsBuff << ",";
          printMemLog(LogsBuff, MemLog);
        });
      }
    });
}

void RISCVConverterSNTF::logHeader(raw_ostream &LogsBuff) const {
  FeatureFlagsSNTF DefaultFeatureFlags;
  auto IsFlagEnabled = [](bool Flag, const char *FlagName) {
    return std::string("# ") + (Flag ? "+" : "-") + FlagName + "\n";
  };
  LogsBuff << "# SNTF\n"
           << "# " << SNTF_VERSION << "\n"
           << IsFlagEnabled(DefaultFeatureFlags.EnableTime, "time")
           << IsFlagEnabled(DefaultFeatureFlags.EnablePC, "pc")
           << IsFlagEnabled(DefaultFeatureFlags.EnableInstrCode, "instr-code")
           << IsFlagEnabled(DefaultFeatureFlags.EnableNextPC, "next-pc")
           << IsFlagEnabled(DefaultFeatureFlags.EnableRegVals,
                            "registers-changed")
           << IsFlagEnabled(DefaultFeatureFlags.EnableMemAccesses,
                            "memory-accesses");
}

void RISCVConverterSNTF::printMemLog(raw_ostream &LogsBuff,
                                     const MemoryLog &MemLog) const {
  LogsBuff << toHexStringTruncate(MemLog.Addr, XLen) << ":";
  switch (MemLog.AccRight) {
  case MemoryLog::AccessType::R:
    LogsBuff << "R";
    break;
  case MemoryLog::AccessType::W:
    LogsBuff << "W";
    break;
  }
  LogsBuff << ":"
           << toHexStringTruncate(MemLog.Value, MemLog.Value.getBitWidth());
}

std::string
RISCVConverterSNTF::getVectorDataSNTF(const VectorRegisterType &VecData) {
  assert(VecData.getBitWidth() % VecElemWidth == 0 &&
         "We will print the vector register by 64-bit elements. We need the "
         "vector register size to be a multiple of 8 bytes.");
  std::string ResultStr;
  llvm::raw_string_ostream StrBuff(ResultStr);
  for (int NumElem = VecData.getBitWidth() / VecElemWidth - 1; NumElem >= 0;
       --NumElem) {
    StrBuff << "[" << NumElem << "]:";
    auto VecElem = VecData.extractBits(VecElemWidth, VecElemWidth * NumElem)
                       .getZExtValue();
    StrBuff << toHexStringTruncate(VecElem, VecElemWidth);
  }
  return ResultStr;
}

void RISCVConverterSNTF::printRegLog(raw_ostream &LogsBuff,
                                     const RegisterLog &RegLog) const {
  switch (RegLog.Type) {
  case RegType::X:
    LogsBuff << "x" << RegLog.RegID << "="
             << toHexStringTruncate(std::get<RegisterType>(RegLog.NewValue),
                                    XLen);
    break;
  case RegType::F: {
    assert(FLen.has_value());
    LogsBuff << "f" << RegLog.RegID << "="
             << toHexStringTruncate(std::get<RegisterType>(RegLog.NewValue),
                                    *FLen);
  } break;
  case RegType::V: {
    LogsBuff << "v" << RegLog.RegID << "="
             << getVectorDataSNTF(
                    std::get<VectorRegisterType>(RegLog.NewValue));
    break;
  }
  default:
    snippy::fatal("Unknown register type");
  }
}

void RISCVConverterSNTF::acceptRecordsAndShiftPC(
    ProgramCounterType PC, ArrayRef<RegisterLog> RegLogs,
    [[maybe_unused]] ArrayRef<MemoryLog> MemLogs) {
  ++Time;
  // Create buffer to print all logs at this time step.
  std::string StrLogsBuff;
  llvm::raw_string_ostream LogsBuff(StrLogsBuff);

  // This is the first instruction. We have nothing to report yet, therefore,
  // instead of a line of logs, we print a header.
  if (Time - 1 == 0) {
    logHeader(LogsBuff);
  } else {
    acceptRecords(LogsBuff, PC, RegLogs, MemLogs);
  }
  // Dump all logs at this iteration into the file.
  *TraceFile << StrLogsBuff;
  PrevPCs.shift(PC);
}

void RISCVConverterSNTF::acceptRecords(raw_ostream &LogsBuff,
                                       ProgramCounterType PC,
                                       ArrayRef<RegisterLog> RL,
                                       ArrayRef<MemoryLog> ML) {
  // This means that the end of the trace has been reached.
  if (PC == LastPC)
    PrevPCs.setFirst(PC);
  RegLogs = RL;
  MemLogs = ML;

  for (auto &LogFunc : Features)
    LogFunc(LogsBuff);
  LogsBuff << "\n";
}

} // namespace snippy
} // namespace llvm
