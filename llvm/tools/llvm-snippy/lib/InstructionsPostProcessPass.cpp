//===-- InstructionsPostProcessPass.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CreatePasses.h"
#include "GeneratorContextPass.h"
#include "InitializePasses.h"

#include "snippy/Generator/LLVMState.h"
#include "snippy/Support/Options.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"
#include "snippy/Target/Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/Format.h"

#include <algorithm>
#include <map>

namespace llvm::snippy {
extern cl::OptionCategory Options;

static snippy::opt<bool>
    VerifyHistogramGen("verify-gen-histogram",
                       cl::desc("Enables verification that resulted "
                                "instructions correspond to input histogram."),
                       cl::cat(Options), cl::Hidden, cl::init(false));

static snippy::opt<double> MaxDeviation(
    "histogram-max-deviation",
    cl::desc("Max deviation in times. If deviation is 0.5 that means that"
             "abs(real_number_of_instrs - expected_number_of_instrs) <= 0.5 * "
             "expected_number_of_instrs."),
    cl::cat(Options), cl::Hidden, cl::init(1.0));

static snippy::opt<bool> MustSucceed(
    "histogram-must-succeed",
    cl::desc(
        "True if llvm-snippy should fail if histogram verifier finds errors"),
    cl::cat(Options), cl::Hidden, cl::init(true));

namespace {

#define DEBUG_TYPE "snippy-inst-postprocess"
#define PASS_DESC "Snippy-inst-postprocess"

class InstructionsPostProcess final : public MachineFunctionPass {
  void collectFreq(const MachineFunction &MF);
  void printData(const MachineFunction &MF) const;
  void stripInstructions(MachineFunction &MF) const;

public:
  static char ID;

  InstructionsPostProcess() : MachineFunctionPass(ID) {
    initializeInstructionsPostProcessPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  std::map<unsigned /*Opcode*/, unsigned /* Count */> TotalInstrs;
  std::map<unsigned /*Opcode*/, unsigned /* Count */> PrimaryInstrs;

  static unsigned getInstCount(const std::map<unsigned, unsigned> &Instrs) {
    return std::accumulate(Instrs.begin(), Instrs.end(), 0,
                           [](unsigned Current, const auto &OpcodeCount) {
                             return Current + OpcodeCount.second;
                           });
  }

  auto getPrimaryInstCount() const { return getInstCount(PrimaryInstrs); }
  auto getTotalInstCount() const { return getInstCount(TotalInstrs); }
};

char InstructionsPostProcess::ID = 0;

} // namespace
} // namespace llvm::snippy

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::InstructionsPostProcess;

INITIALIZE_PASS_BEGIN(InstructionsPostProcess, DEBUG_TYPE, PASS_DESC, true,
                      false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_END(InstructionsPostProcess, DEBUG_TYPE, PASS_DESC, true, false)
namespace llvm {
MachineFunctionPass *createInstructionsPostProcessPass() {
  return new InstructionsPostProcess();
}

namespace snippy {

void InstructionsPostProcess::stripInstructions(MachineFunction &MF) const {
  // Reset all pc sections metadata that used as support mark.
  for (MachineBasicBlock &MBB : MF) {
    for (auto &Instr : MBB.instrs()) {
      Instr.setPCSections(MF, nullptr);
    }
  }
}

void InstructionsPostProcess::collectFreq(const MachineFunction &MF) {
  for (const auto &MBB : MF) {
    for (const auto &Instr : MBB.instrs()) {
      if (Instr.isPseudo()) {
        continue;
      }
      if (!checkSupportMetadata(Instr)) {
        ++PrimaryInstrs[Instr.getOpcode()];
      }
      ++TotalInstrs[Instr.getOpcode()];
    }
  }
}

template <typename ValueTy>
static inline ValueTy getValueOrZero(const std::map<unsigned, ValueTy> Map,
                                     unsigned Opcode) {
  return Map.count(Opcode) ? Map.at(Opcode) : 0;
}

void InstructionsPostProcess::printData(const MachineFunction &MF) const {
  GeneratorContextWrapper &CtxWrapper = getAnalysis<GeneratorContextWrapper>();
  const GeneratorContext &SGCtx = CtxWrapper.getContext();
  const OpcodeCache &OpCache = SGCtx.getOpcodeCache();

  auto ExpectedDist = SGCtx.createDefaultOpcodeGenerator()->getProbabilities();

  auto &Output = outs();
  constexpr unsigned OpcodeWidth = 12;
  constexpr unsigned CountWidth = 8;
  constexpr unsigned GenCountWidth = 15;
  constexpr unsigned ObservedProbWidth = 15;
  constexpr unsigned GenProbWidth = 15;
  constexpr unsigned ExpectedProbWidth = 25;
  constexpr unsigned StatusWidth = 8;
  Output << "Start Verification for function: " << MF.getName() << ":\n";
  Output << "Start generated instructions statistics:\n";
  Output << right_justify("Opcode", OpcodeWidth)
         << right_justify("Count", CountWidth)
         << right_justify("Primary_Count", GenCountWidth)
         << right_justify("Freq,%", ObservedProbWidth)
         << right_justify("Primary_Freq,%", GenProbWidth)
         << right_justify("Expected_Prob,%", ExpectedProbWidth)
         << right_justify("Status", StatusWidth) << "\n";

  auto TotalInstCount = getTotalInstCount();
  auto PrimaryInstCount = getPrimaryInstCount();
  // If any of this is zero, we will have division by zero.
  if (TotalInstCount == 0)
    TotalInstCount = 1;
  if (PrimaryInstCount == 0)
    PrimaryInstCount = 1;

  bool GenerationGood = true;

  for (const auto [Opcode, TotalCount] : TotalInstrs) {
    double ObservedFreq =
        100 * static_cast<double>(TotalCount) / TotalInstCount;
    double PrimaryCurrentInstCount = getValueOrZero(PrimaryInstrs, Opcode);
    double PrimaryFreq = 100 * PrimaryCurrentInstCount / PrimaryInstCount;
    double ExpectedProb = 100 * getValueOrZero(ExpectedDist, Opcode);

    double ExpectedNum =
        getValueOrZero(ExpectedDist, Opcode) * PrimaryInstCount;
    double CurrentInstMaxDeviationNum = ExpectedNum * MaxDeviation;

    bool IsWithingThreshold = std::abs(PrimaryCurrentInstCount - ExpectedNum) <=
                              CurrentInstMaxDeviationNum;
    GenerationGood &= IsWithingThreshold;
    Output << right_justify(OpCache.name(Opcode), OpcodeWidth)
           << format_decimal(TotalCount, CountWidth)
           << format_decimal(getValueOrZero(PrimaryInstrs, Opcode),
                             GenCountWidth)
           << format("%*.1f", ObservedProbWidth, ObservedFreq)
           << format("%*.1f", GenProbWidth, PrimaryFreq)
           << format("%*.1f", ExpectedProbWidth, ExpectedProb)
           << right_justify(IsWithingThreshold ? "OK" : "FAILED", StatusWidth)
           << "\n";
  }
  Output << "End generatated instructions statistics\n";
  // check that all opcodes from histogram appear in TotalInstrs.
  std::map<unsigned, double> MissedOpcodes;
  std::set_difference(ExpectedDist.begin(), ExpectedDist.end(),
                      TotalInstrs.begin(), TotalInstrs.end(),
                      std::inserter(MissedOpcodes, MissedOpcodes.end()),
                      [](const std::pair<unsigned, double> &Lhs,
                         const std::pair<unsigned, unsigned> &Rhs) {
                        return Lhs.first < Rhs.first;
                      });
  bool HaveMissedOpcodes = !MissedOpcodes.empty();
  if (HaveMissedOpcodes)
    Output << "ERROR: some opcodes from histogram not generated:\n"
           << right_justify("Opcode", OpcodeWidth)
           << right_justify("Probability,%", ExpectedProbWidth) << "\n";

  for (const auto &[Opcode, Prob] : MissedOpcodes) {
    double ExpectedProb = 100 * Prob;
    Output << right_justify(OpCache.name(Opcode), OpcodeWidth)
           << format("%*.1f", ExpectedProbWidth, ExpectedProb) << "\n";
  }
  if (HaveMissedOpcodes)
    Output << "End information for non-generated opcodes\n";
  Output << "Number of support instructions: "
         << TotalInstCount - PrimaryInstCount << "\n";
  Output << "Number of primary instructions: " << PrimaryInstCount << "\n";
  Output << "Total number of instructions: " << TotalInstCount << "\n";
  Output << "Percent of support instructions: "
         << format("%2.1f", 100 * (TotalInstCount - PrimaryInstCount) /
                                static_cast<double>(TotalInstCount))
         << "\n";
  Output << "End Verification for function: " << MF.getName() << "\n";
  if (MustSucceed && (HaveMissedOpcodes || !GenerationGood))
    report_fatal_error("Snippy's output does not correspond to histogram",
                       false);
}

bool InstructionsPostProcess::runOnMachineFunction(MachineFunction &MF) {
  if (VerifyHistogramGen)
    collectFreq(MF);
  stripInstructions(MF);
  if (VerifyHistogramGen)
    printData(MF);
  return false;
}

} // namespace snippy
} // namespace llvm
