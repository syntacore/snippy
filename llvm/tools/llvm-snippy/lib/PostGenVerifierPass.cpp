//===-- PostGenVerifierPass.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InitializePasses.h"

#include "snippy/Generator/BlockGenPlanWrapperPass.h"
#include "snippy/Generator/GeneratorContextPass.h"

#include "snippy/Support/Options.h"

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

static snippy::opt<bool>
    VerifyGenPlan("verify-gen-plan",
                  cl::desc("Enables verification that resulted instructions "
                           "correspond to generation plan."),
                  cl::cat(Options), cl::Hidden, cl::init(false));

namespace {

#define DEBUG_TYPE "snippy-post-verification"
#define PASS_DESC "Snippy-post-verification"

class PostGenVerifier final : public MachineFunctionPass {
  void collectFreq(const MachineFunction &MF, const SnippyTarget &SnippyTgt);
  void verifyGenPlan(const MachineFunction &MF, LLVMContext &LLVMCtx,
                     const GeneratorContext &SGCtx) const;
  void printData(const MachineFunction &MF) const;

public:
  static char ID;

  PostGenVerifier() : MachineFunctionPass(ID) {
    initializePostGenVerifierPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return PASS_DESC " Pass"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GeneratorContextWrapper>();
    AU.addRequired<BlockGenPlanWrapper>();
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

char PostGenVerifier::ID = 0;

} // namespace
} // namespace llvm::snippy

using llvm::callDefaultCtor;
using llvm::PassInfo;
using llvm::PassRegistry;
using llvm::snippy::PostGenVerifier;

INITIALIZE_PASS_BEGIN(PostGenVerifier, DEBUG_TYPE, PASS_DESC, true, false)
INITIALIZE_PASS_DEPENDENCY(GeneratorContextWrapper)
INITIALIZE_PASS_DEPENDENCY(BlockGenPlanWrapper)
INITIALIZE_PASS_END(PostGenVerifier, DEBUG_TYPE, PASS_DESC, true, false)
namespace llvm {
MachineFunctionPass *createPostGenVerifierPass() {
  return new PostGenVerifier();
}

namespace snippy {

void PostGenVerifier::collectFreq(const MachineFunction &MF,
                                  const SnippyTarget &SnippyTgt) {
  for (const auto &MBB : MF) {
    for (const auto &Instr : MBB.instrs()) {
      if (Instr.isPseudo() && !SnippyTgt.isPseudoAllowed(Instr.getOpcode())) {
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

void PostGenVerifier::printData(const MachineFunction &MF) const {
  GeneratorContextWrapper &CtxWrapper = getAnalysis<GeneratorContextWrapper>();
  const GeneratorContext &SGCtx = CtxWrapper.getContext();
  const OpcodeCache &OpCache = SGCtx.getProgramContext().getOpcodeCache();

  auto ExpectedDist =
      SGCtx.getGenSettings().createDefaultOpcodeGenerator()->getProbabilities();

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
  Output << "End generated instructions statistics\n";
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
    snippy::fatal("Snippy's output does not correspond to histogram");
}

enum class VerificationStatus {
  EverythingGood,
  DoNotCorrespondPlan,
  UnalignedSizeLimitFound
};

static VerificationStatus
verifyBBWithNumLimit(const MachineBasicBlock &MBB,
                     const planning::BasicBlockRequest &BBReq) {
  // We don't count branches, because they are terminated instructions, which
  // are not mentioned in gen plan
  size_t Count =
      std::count_if(MBB.instr_begin(), MBB.instr_end(), [](const auto &Instr) {
        return !checkSupportMetadata(Instr) && !Instr.isBranch();
      });
  size_t Planned = BBReq.limit().getLimit();
  if (Planned != Count) {
    outs() << printMBBReference(MBB) << " : count -- " << Count
           << ", planned -- " << Planned << "\n";
    return VerificationStatus::DoNotCorrespondPlan;
  }
  return VerificationStatus::EverythingGood;
}

static VerificationStatus verifyBBWithSizeLimit(
    const MachineBasicBlock &MBB, const planning::BasicBlockRequest &BBReq,
    const planning::FunctionRequest &FunReq, const GeneratorContext &SGCtx) {
  // in the last block, not counting __snippy_exit
  assert(!MBB.empty());
  auto InstrEnd =
      (MBB.getNextNode() == nullptr) ? --MBB.instr_end() : MBB.instr_end();

  auto &State = SGCtx.getProgramContext().getLLVMState();
  size_t Count = State.getCodeBlockSize(MBB.instr_begin(), InstrEnd);
  size_t Planned = BBReq.limit().getLimit();

  // There are some instructions (like branches) which are not included in
  // BBReq.limit() because they were created before the generation pass.
  // Information about these instructions is stored in
  // InstructionGroupRequests inside BBReq
  Planned += std::accumulate(FunReq.at(&MBB).begin(), FunReq.at(&MBB).end(), 0,
                             [&](size_t Lhs, const auto &Rhs) {
                               return Lhs + Rhs.initialStats().GeneratedSize;
                             });

  const auto &SnippyTgt = State.getSnippyTarget();
  unsigned MinInstrSize = *SnippyTgt
                               .getPossibleInstrsSize(State.getSubtargetImpl(
                                   MBB.getParent()->getFunction()))
                               .begin();
  if (Planned > Count && Planned - Count < MinInstrSize) {
    outs() << printMBBReference(MBB) << " :  size -- " << Count
           << ", planned -- " << Planned << "\n";
    return VerificationStatus::UnalignedSizeLimitFound;
  } else if (Planned != Count) {
    outs() << printMBBReference(MBB) << " :  size -- " << Count
           << ", planned -- " << Planned << "\n";
    return VerificationStatus::DoNotCorrespondPlan;
  }
  return VerificationStatus::EverythingGood;
}

static VerificationStatus
verifyBasicBlock(const MachineBasicBlock &MBB,
                 const planning::FunctionRequest &FunReq,
                 const GeneratorContext &SGCtx) {
  auto &BBReq = FunReq.at(&MBB);

  if (BBReq.limit().isNumLimit()) {
    return verifyBBWithNumLimit(MBB, BBReq);
  } else if (BBReq.limit().isSizeLimit()) {
    return verifyBBWithSizeLimit(MBB, BBReq, FunReq, SGCtx);
  }
  // For now, there can't be a mixed limit in a BBReq
  else {
    llvm_unreachable("unknown verification limit");
  }
}

void PostGenVerifier::verifyGenPlan(const MachineFunction &MF,
                                    LLVMContext &LLVMCtx,
                                    const GeneratorContext &SGCtx) const {
  const auto &FunReq =
      getAnalysis<BlockGenPlanWrapper>().getFunctionRequest(&MF);
  bool UnalignedSizeLimitFound = false;
  bool GenPlanCorrespondence = true;

  for (const auto &MBB : MF) {
    auto VerifStatus = verifyBasicBlock(MBB, FunReq, SGCtx);
    if (VerifStatus == VerificationStatus::DoNotCorrespondPlan)
      GenPlanCorrespondence = false;
    else if (VerifStatus == VerificationStatus::UnalignedSizeLimitFound)
      UnalignedSizeLimitFound = true;
  }

  if (UnalignedSizeLimitFound)
    snippy::warn(WarningName::GenPlanVerification, LLVMCtx,
                 "request for " + MF.getName() +
                     " contains size limits which are impossible to satisfy",
                 "size of some blocks can differ from plan");

  if (!GenPlanCorrespondence)
    snippy::fatal(LLVMCtx, "gen plan verification failed",
                  "snippy's output does not correspond to generation plan");
}

bool PostGenVerifier::runOnMachineFunction(MachineFunction &MF) {
  auto &SGCtx = getAnalysis<GeneratorContextWrapper>().getContext();
  auto &ProgCtx = SGCtx.getProgramContext();
  const auto &SnippyTgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto &LLVMCtx = ProgCtx.getLLVMState().getCtx();
  if (VerifyHistogramGen)
    collectFreq(MF, SnippyTgt);
  if (VerifyGenPlan)
    verifyGenPlan(MF, LLVMCtx, SGCtx);
  if (VerifyHistogramGen)
    printData(MF);
  return false;
}

} // namespace snippy
} // namespace llvm
