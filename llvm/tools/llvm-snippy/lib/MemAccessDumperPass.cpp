//===-- MemAccessDumperPass.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/CreatePasses.h"
#include "snippy/Generator/InstructionGeneratorPass.h"
#include "snippy/Support/Options.h"

#define DEBUG_TYPE "snippy-mem-access-dump"
#define PASS_DESC "Snippy Memory Access Dumper"

namespace llvm {
namespace snippy {

extern cl::OptionCategory Options;

static snippy::opt<std::string> DumpMemAccesses(
    "dump-memory-accesses",
    cl::desc("Dump memory addresses that are accessed by instructions in the "
             "snippet to a file creating `access-addresses` memory scheme. It "
             "does not record auxiliary instructions (e.g. selfcheck). Pass "
             "the options with an empty value to dump to stdout."),
    cl::value_desc("filename"), cl::cat(Options));
static snippy::opt<std::string> DumpMemAccessesRestricted(
    "dump-memory-accesses-restricted",
    cl::desc(
        "Dump memory addresses that are accessed by instructions in the "
        "snippet to a file creating `restricted-addresses` memory scheme. It "
        "does not record auxiliary instructions (e.g. selfcheck). Pass "
        "the options with an empty value to dump to stdout."),
    cl::value_desc("filename"), cl::cat(Options));
static snippy::opt<bool> DumpBurstPlainAccesses(
    "dump-burst-accesses-as-plain-addrs",
    cl::desc("Dump memory addresses accessed by burst groups as plain "
             "addresses rather than ranges descriptions"),
    cl::cat(Options), cl::init(false));

namespace {

bool dumpPlainAccesses(raw_ostream &OS, const PlainAccessesType &Accesses,
                       bool Restricted, StringRef Prefix, bool Append) {
  if (Accesses.empty())
    return false;

  if (!Append)
    OS << Prefix.data() << "plain:\n";

  for (auto [Addr, AccessSize] : Accesses) {
    OS << "      - addr: 0x" << Twine::utohexstr(Addr).str() << "\n";
    if (Restricted)
      OS << "        access-size: " << AccessSize << "\n";
  }
  return true;
}

static void dumpBurstAccesses(raw_ostream &OS,
                              const BurstGroupAccessesType &Accesses,
                              StringRef Prefix) {
  if (Accesses.empty())
    return;

  OS << Prefix.data() << "burst:\n";

  for (const auto &BurstDesc : Accesses) {
    assert(!BurstDesc.empty() && "At least one range is expected");
    for (auto &RangeDesc : BurstDesc) {
      assert(RangeDesc.MinOffset <= RangeDesc.MaxOffset);
      bool IsMinNegative = RangeDesc.MinOffset < 0;
      MemAddr MinOffsetAbs = std::abs(RangeDesc.MinOffset);
      bool IsMaxNegative = RangeDesc.MaxOffset < 0;
      MemAddr MaxOffsetAbs = std::abs(RangeDesc.MaxOffset);
      assert(!IsMinNegative || RangeDesc.Address >= MinOffsetAbs);
      assert(IsMinNegative ||
             std::numeric_limits<MemAddr>::max() - RangeDesc.Address >=
                 MinOffsetAbs);
      auto BaseAddr = IsMinNegative ? RangeDesc.Address - MinOffsetAbs
                                    : RangeDesc.Address + MinOffsetAbs;
      MemAddr Size = 0;
      if (IsMaxNegative) {
        assert(MinOffsetAbs >= MaxOffsetAbs);
        Size = MinOffsetAbs - MaxOffsetAbs;
      } else if (IsMinNegative) {
        assert(std::numeric_limits<MemAddr>::max() - MaxOffsetAbs >=
               MinOffsetAbs);
        Size = MaxOffsetAbs + MinOffsetAbs;
      } else {
        assert(MaxOffsetAbs >= MinOffsetAbs);
        Size = MaxOffsetAbs - MinOffsetAbs;
      }
      assert(std::numeric_limits<MemAddr>::max() - Size >=
             RangeDesc.AccessSize);
      Size += RangeDesc.AccessSize;
      OS << "        - addr: 0x" << Twine::utohexstr(BaseAddr).str() << "\n";
      OS << "          size: " << Size << "\n";
      OS << "          stride: " << RangeDesc.MinStride << "\n";
      OS << "          access-size: " << RangeDesc.AccessSize << "\n";
    }
  }
}

void dumpMemAccesses(StringRef Filename, const PlainAccessesType &Plain,
                     const BurstGroupAccessesType &BurstRanges,
                     const PlainAccessesType &BurstPlain, bool Restricted) {
  if (Plain.empty() && BurstRanges.empty() && BurstPlain.empty()) {
    snippy::warn(WarningName::MemoryAccess, "Cannot dump memory accesses",
                 "No accesses were generated, file won't be created.");
    return;
  }

  std::string DumpStr;
  raw_string_ostream SS(DumpStr);
  SS << (Restricted ? "restricted-addresses:\n" : "access-addresses:\n");
  if (!Restricted)
    SS << "  - ordered: true\n";

  constexpr auto NewEntry = "  - ";
  constexpr auto SameEntry = "    ";
  auto Prefix = Restricted ? NewEntry : SameEntry;

  auto Added =
      dumpPlainAccesses(SS, Plain, Restricted, Prefix, /* Append */ false);
  if (Added)
    Prefix = SameEntry;

  if (!Restricted && !DumpBurstPlainAccesses)
    dumpBurstAccesses(SS, BurstRanges, Prefix);
  else
    dumpPlainAccesses(SS, BurstPlain, Restricted, Prefix, /* Append */ Added);

  if (Filename.empty())
    outs() << SS.str();
  else
    writeFile(Filename, DumpStr);
}

struct MemoryAccessDumper final : public MachineFunctionPass {
  static char ID;

  MemoryAccessDumper() : MachineFunctionPass(ID){};

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<InstructionGenerator>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char MemoryAccessDumper::ID = 0;

} // namespace
} // namespace snippy
} // namespace llvm

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::MemoryAccessDumper;

INITIALIZE_PASS(MemoryAccessDumper, DEBUG_TYPE, PASS_DESC, false, false)

namespace llvm {

MachineFunctionPass *createMemAccessDumperPass() {
  return new MemoryAccessDumper();
}

namespace snippy {

bool MemoryAccessDumper::runOnMachineFunction(MachineFunction &MF) {
  auto &MAI = getAnalysis<InstructionGenerator>().get<MemAccessInfo>(MF);

  if (DumpMemAccesses.isSpecified())
    dumpMemAccesses(DumpMemAccesses.getValue(), MAI.getMemAccesses(),
                    MAI.getBurstRangeAccesses(), MAI.getBurstPlainAccesses(),
                    /* Restricted */ false);

  if (DumpMemAccessesRestricted.isSpecified())
    dumpMemAccesses(DumpMemAccessesRestricted.getValue(), MAI.getMemAccesses(),
                    MAI.getBurstRangeAccesses(), MAI.getBurstPlainAccesses(),
                    /* Restricted */ true);

  return false;
}

} // namespace snippy
} // namespace llvm
