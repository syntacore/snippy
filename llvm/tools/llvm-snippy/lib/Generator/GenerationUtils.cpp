//===-- GenerationUtils.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Config/Config.h"
#include "snippy/Config/RegisterAccess.h"
#include "snippy/Generator/MemAccessInfo.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SMCManager.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Generator/TopMemAccSampler.h"
#include "snippy/Support/Options.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/Support/Debug.h"

#include <functional>

namespace llvm {
namespace snippy {
extern cl::OptionCategory Options;

static snippy::opt<unsigned> BurstAddressRandomizationThreshold(
    "burst-addr-rand-threshold",
    cl::desc(
        "Number of attempts to randomize address of each instruction in the "
        "burst group"),
    cl::Hidden, cl::init(100));

// For the given AddressInfo and AddressRestriction try to find another
// AddressInfo such that:
// 1. New AddressInfo will cover different set of addresses (for better
// addresses and offsets randomization) in burst groups
// 2. Base address from the given AddressInfo may be reused for the new one. It
// means that the provided AddressRestriction allows immediate offsets that are
// necessary for the base address change.
//
// Example:
// Given AddressInfo is: base address = 10, min offset = 0, max offset = 0 -->
// effective address is 10 Given AddressRestriction on immediate offsets are:
// min = -5, max = 5
//
// We are limited in the number of registers we can use for base addresses in
// burst groups, but the given AddressInfo isn't really interesting: it doesn't
// allow immediate offsets other than zero. So, we'd like to choose another
// address info that provides wider range of immediate offsets and can use the
// original base address under the given AddressRestrictions.
//
// Let's say we've choosen a new AddressInfo: base address = 15, min offset =
// -10, max offset = 10 --> effective addresses are [5, 25] Replacement of the
// base address gives: base address = 10, min offset = -5 , max offset = 15 -->
// effective addresses are still the same [5, 25] Then we need to apply
// AddressRestrictions: base address = 10, min offset = -5 , max offset = 5 -->
// effective addresses were shrinked to [5, 15]
AddressInfo
selectAddressForSingleInstrFromBurstGroup(InstructionGenerationContext &IGC,
                                          AddressInfo OrigAI,
                                          const AddressRestriction &OpcodeAR) {
  if (OrigAI.MinOffset != 0 || OrigAI.MaxOffset != 0 ||
      (OpcodeAR.ImmOffsetRange.getMin() == 0 &&
       OpcodeAR.ImmOffsetRange.getMax() == 0)) {
    // Either OrigAI allows non-zero offets, or address restrictions for the
    // given opcode doesn't allow non-zero offsets. In both cases there is
    // nothing to change.
    return OrigAI;
  }

  auto &MS = IGC.getMemoryAccessSampler();
  auto OrigAddr = OrigAI.Address;
  assert(OpcodeAR.Opcodes.size() == 1 &&
         "Expected AddressRestriction only for one opcode");
  for (unsigned i = 0; i < BurstAddressRandomizationThreshold; ++i) {
    AddressGenInfo AddrGenInfo{OpcodeAR.AccessSize, OpcodeAR.AccessAlignment,
                               OpcodeAR.AllowMisalign, /*Burst=*/true};
    auto CandidateAccess = MS.sample(AddrGenInfo);
    if (!CandidateAccess) {
      std::string PrefixErr;
      raw_string_ostream OS(PrefixErr);
      OS << "Cannot sample memory access for single instruction from burst "
            "group";
      snippy::fatal(PrefixErr, toString(CandidateAccess.takeError()));
    }
    auto &CandidateAI = *CandidateAccess;

    auto Stride = std::lcm<int64_t, int64_t>(
        std::max<int64_t>(OpcodeAR.ImmOffsetRange.getStride(),
                          CandidateAI.MinStride),
        OpcodeAR.OffsetAlignment);
    CandidateAI.MinStride = std::max<int64_t>(Stride, 1);

    if (CandidateAI.Address == OrigAddr && CandidateAI.MinOffset == 0 &&
        CandidateAI.MaxOffset == 0)
      continue;

    bool IsDiffNeg = OrigAddr >= CandidateAI.Address;
    auto AbsDiff = IsDiffNeg ? OrigAddr - CandidateAI.Address
                             : CandidateAI.Address - OrigAddr;
    auto SMax = std::numeric_limits<decltype(CandidateAI.MinOffset)>::max();
    auto SMin = std::numeric_limits<decltype(CandidateAI.MinOffset)>::min();
    assert(SMax > 0);
    // We are going to apply Diff to the signed type. Check that it fits.
    if (!IsDiffNeg && AbsDiff > static_cast<decltype(AbsDiff)>(SMax))
      continue;
    if (IsDiffNeg &&
        AbsDiff > static_cast<decltype(AbsDiff)>(std::abs(SMin + 1)))
      continue;

    auto SDiff = static_cast<decltype(CandidateAI.MinOffset)>(AbsDiff);
    if (IsDiffNeg)
      SDiff = -SDiff;

    if (IsSAddOverflow(SDiff, CandidateAI.MinOffset) ||
        IsSAddOverflow(SDiff, CandidateAI.MaxOffset))
      continue;

    auto AlignedMinOffset =
        alignSignedTo(CandidateAI.MinOffset + SDiff, CandidateAI.MinStride);
    auto AlignedMaxOffset =
        alignSignedDown(CandidateAI.MaxOffset + SDiff, CandidateAI.MinStride);
    if (AlignedMaxOffset < AlignedMinOffset ||
        (AlignedMinOffset == 0 && AlignedMaxOffset == 0))
      continue;

    if (OpcodeAR.ImmOffsetRange.getMin() <= AlignedMinOffset &&
        AlignedMinOffset <= OpcodeAR.ImmOffsetRange.getMax() &&
        AlignedMinOffset <= AlignedMaxOffset &&
        (CandidateAI.Address + CandidateAI.MinOffset) % CandidateAI.MinStride ==
            (OrigAddr + AlignedMinOffset) % CandidateAI.MinStride) {
      CandidateAI.Address = OrigAddr;
      CandidateAI.MinOffset = AlignedMinOffset;
      CandidateAI.MaxOffset = AlignedMaxOffset;
      return CandidateAI;
    }
  }
  return OrigAI;
}

std::map<unsigned, APInt> selectOperandsForConsecutiveInstrs(
    InstructionGenerationContext &InstrGenCtx, const SnippyTarget &Tgt,
    RegPoolWrapper &RP,
    std::vector<planning::InstructionRequest> &BurstInstrs) {
  auto &State = InstrGenCtx.ProgCtx.getLLVMState();
  auto IsMemUser = [&Tgt](auto Opc) -> bool {
    return Tgt.countAddrsToGenerate(Opc);
  };
  std::vector<unsigned> MemUsers;
  MemUsers.reserve(BurstInstrs.size());
  copy_if(map_range(BurstInstrs, [](auto &&IR) { return IR.Opcode; }),
          std::back_inserter(MemUsers), IsMemUser);
  auto [MemUserIdxToPreselectedOps, RegsToInit] =
      selectOperandsForMemoryInstructions(InstrGenCtx, MemUsers, RP);
  // Here we collected all registers that should be initialized (we don't
  // initialize registers for non-memory instructions). Initialize them all in
  // one go.
  unsigned MemUsersIdx = 0;
  for (auto &&Instr : BurstInstrs) {
    if (IsMemUser(Instr.Opcode)) {
      Instr.Preselected = MemUserIdxToPreselectedOps[MemUsersIdx++];
    } else {
      // For instructions that do not use memory we can simply preselect their
      // operands.
      auto &II = State.getInstrInfo();
      auto &InstrDesc = II.get(Instr.Opcode);
      Instr.Preselected.resize(InstrDesc.getNumOperands());
      // To avoid spoling registers used in memory instruction we use same
      // register pool and mark all initialized registers as excluded
      DenseSet<Register> Excluded;
      Excluded.insert_range(make_first_range(RegsToInit));
      selectNonMemoryOperands(InstrDesc, Instr.Preselected, InstrGenCtx, RP,
                              Excluded);
    }
  }
  return RegsToInit;
}

// NumDefs + NumAddrs might be more than a number of available regs. This
// normalizes the number of regs to reserve for addrs.
unsigned normalizeNumRegs(unsigned NumDefs, unsigned NumAddrs,
                          unsigned NumRegs) {
  if (NumRegs == 0)
    snippy::fatal("No registers left to reserve for burst mode");
  auto Ratio = 1.0 * NumRegs / (NumAddrs + NumDefs);
  if (Ratio > 1.0)
    return NumAddrs;
  unsigned NumAddrRegsToGen = Ratio * NumAddrs;
  assert(NumAddrRegsToGen + Ratio * NumDefs <= NumRegs &&
         "Wrong number of registers to reserve");
  return NumAddrRegsToGen;
}

// Count how many def regs of a register class RC the instruction has.
unsigned countDefsHavingRC(ArrayRef<unsigned> Opcodes,
                           const TargetRegisterInfo &RegInfo,
                           const TargetRegisterClass &RC,
                           const MCInstrInfo &InstrInfo) {
  auto CountDefsForOpcode = [&](unsigned Init, unsigned Opcode) {
    const auto &InstrDesc = InstrInfo.get(Opcode);
    auto NumDefs = InstrDesc.getNumDefs();
    auto DefBegin = InstrDesc.operands().begin();
    auto DefEnd = std::next(DefBegin, NumDefs);
    return Init +
           std::count_if(DefBegin, DefEnd, [&](const MCOperandInfo &OpInfo) {
             const auto *OpRC = RegInfo.getRegClass(OpInfo.RegClass);
             return RC.hasSubClassEq(OpRC);
           });
  };
  return std::accumulate(Opcodes.begin(), Opcodes.end(), 0u,
                         CountDefsForOpcode);
}

unsigned countAddrs(ArrayRef<unsigned> Opcodes, const SnippyTarget &SnippyTgt) {
  auto CountAddrsForOpcode = [&SnippyTgt](unsigned Init, unsigned Opcode) {
    return Init + SnippyTgt.countAddrsToGenerate(Opcode);
  };
  return std::accumulate(Opcodes.begin(), Opcodes.end(), 0u,
                         CountAddrsForOpcode);
}

// For the given InstrDesc fill the vector of selected operands to account them
// in instruction generation procedure.
planning::PreselectedOperands selectMemoryOperands(const MCInstrDesc &InstrDesc,
                                                   unsigned BaseReg,
                                                   const AddressInfo &AI) {
  planning::PreselectedOperands Preselected;
  for (const auto &MCOpInfo : InstrDesc.operands()) {
    if (MCOpInfo.OperandType == MCOI::OperandType::OPERAND_MEMORY)
      Preselected.emplace_back(BaseReg);
    else if (MCOpInfo.OperandType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
      // FIXME: Here we just use the fact that RISC-V loads and stores from base
      // subset have only one target specific operand that is offset.
      auto MinStride = AI.MinStride;
      if (MinStride == 0)
        MinStride = 1;
      Preselected.emplace_back(
          StridedImmediate(AI.MinOffset, AI.MaxOffset, MinStride));
    } else
      Preselected.emplace_back();
  }
  return Preselected;
}

void selectNonMemoryOperands(
    const MCInstrDesc &InstrDesc,
    SmallVectorImpl<planning::PreselectedOpInfo> &Preselected,
    planning::InstructionGenerationContext &InstrGenCtx, RegPoolWrapper &RP,
    const DenseSet<Register> &Excluded,
    const DenseSet<Register> &Destinations) {
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  auto &Tgt = State.getSnippyTarget();
  auto &RI = State.getRegInfo();
  auto &II = State.getInstrInfo();
  auto &RegGen = ProgCtx.getRegGen();
  assert(Preselected.size() == InstrDesc.getNumOperands());
  // Temporary reg pool to take implicit register restrictions into account.
  // E.g. vluxei instructions cannot have overlapping registers with different
  // element sizes
  auto TmpRP = InstrGenCtx.pushRegPool();
  for (auto &&[Idx, OpInfo] : enumerate(InstrDesc.operands())) {
    if (!Tgt.shouldPreselectOperandInBurstMode(InstrDesc, Idx))
      continue;
    auto TiedTo = InstrDesc.getOperandConstraint(Idx, MCOI::TIED_TO);
    if (TiedTo >= 0) {
      Preselected[Idx].setTiedTo(TiedTo);
      continue;
    }
    bool IsDst = Idx < InstrDesc.getNumDefs();
    auto RegClass = Tgt.getRegClass(InstrGenCtx, OpInfo.RegClass, Idx,
                                    InstrDesc.getOpcode(), RI);
    AccessMaskBit Mask =
        IsDst ? AccessMaskBit::PrimaryW : AccessMaskBit::PrimaryR;
    auto CustomMask = Tgt.getCustomAccessMaskForOperand(InstrDesc, Idx);
    if (CustomMask != AccessMaskBit::None)
      Mask = CustomMask;
    auto ExcludedForOperand =
        Tgt.excludeRegsForOperand(InstrGenCtx, RegClass, InstrDesc, Idx);
    copy(Excluded, std::back_inserter(ExcludedForOperand));
    if (!IsDst)
      copy(Destinations, std::back_inserter(ExcludedForOperand));
    auto Include = Tgt.includeRegs(InstrDesc.getOpcode(), RegClass);
    auto ExpectedReg =
        RegGen.generate(RegClass, OpInfo.RegClass, RI, *TmpRP, InstrGenCtx.MBB,
                        Tgt, ExcludedForOperand, Include, Mask);
    auto ReportCouldNotSelectReg = [&]() {
      snippy::fatal(
          formatv("Could not select register for \"{0}\" in burst group",
                  II.getName(InstrDesc.getOpcode())),
          "try reducing burst group size and relaxing register reservation");
    };
    if (auto Err = ExpectedReg.takeError()) {
      consumeError(std::move(Err));
      ReportCouldNotSelectReg();
    }
    auto SelectedReg = *ExpectedReg;
    auto FirstReg = SelectedReg;
    if (!Tgt.isPhysRegClass(RegClass.getID(), RI))
      FirstReg = Tgt.getFirstPhysReg(SelectedReg, RI);
    // This handles situations where selected register cannot be reused as
    // another operand of the same instruction
    Tgt.reserveRegsIfNeeded(InstrGenCtx, InstrDesc.getOpcode(), IsDst,
                            /*isMem=*/false, FirstReg);
    Preselected[Idx] = Register(SelectedReg);
  }
}

static DenseSet<Register> getExcludedRegsForOpcodes(ArrayRef<unsigned> Opcodes,
                                                    const LLVMState &State) {
  const auto &Tgt = State.getSnippyTarget();
  const auto &RI = State.getRegInfo();
  const auto &II = State.getInstrInfo();
  DenseSet<Register> Exclude;
  SmallVector<Register> ExcludedRegs;
  for (auto Opcode : Opcodes) {
    Tgt.excludeFromMemRegsForInstr(II.get(Opcode), RI, ExcludedRegs);
    Exclude.insert(ExcludedRegs.begin(), ExcludedRegs.end());
  }
  return Exclude;
}

static std::optional<int>
getOffsetImmediate(ArrayRef<planning::PreselectedOpInfo> Preselected) {
  auto Found =
      find_if(Preselected, [](auto &OpInfo) { return OpInfo.isImm(); });
  if (Found == Preselected.end())
    return std::nullopt;
  auto Imm = Found->getImm();
  assert(Imm.getMax() == Imm.getMin());
  return Imm.getMax();
}

std::pair<std::vector<planning::PreselectedOperands>, std::map<unsigned, APInt>>
selectOperandsForMemoryInstructions(InstructionGenerationContext &InstrGenCtx,
                                    ArrayRef<unsigned> Opcodes,
                                    RegPoolWrapper &RP) {
  unsigned Count = Opcodes.size();
  const auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &State = ProgCtx.getLLVMState();
  const auto &Tgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &RegInfo = State.getRegInfo();
  auto OpcodeIdxToBaseReg = generateBaseRegs(InstrGenCtx, Opcodes);
  for (auto R : OpcodeIdxToBaseReg)
    RP.addReserved(R, AccessMaskBit::RW);
  auto [RegsToInit, OpcodeIdxToAI] =
      mapOpcodeIdxToAI(InstrGenCtx, OpcodeIdxToBaseReg, Opcodes);
  // We already initialized base registers. Now to select other register
  // operands we must exclude base ones because they can't be modified again.
  auto Excluded = getExcludedRegsForOpcodes(Opcodes, State);
  Excluded.insert_range(make_first_range(RegsToInit));
  assert(OpcodeIdxToBaseReg.size() == Count);
  assert(OpcodeIdxToAI.size() == Count);
  std::vector<planning::PreselectedOperands> OpcodeIdxToPreselectedOps(Count);
  DenseSet<Register> Destinations;
  for (unsigned Idx = 0; Idx < Count; ++Idx) {
    auto Opcode = Opcodes[Idx];
    auto BaseReg = OpcodeIdxToBaseReg[Idx];
    auto &AI = OpcodeIdxToAI[Idx];
    auto &Preselected = OpcodeIdxToPreselectedOps[Idx];
    assert(Tgt.countAddrsToGenerate(Opcode));
    const auto &InstrDesc = InstrInfo.get(Opcode);
    // Select memory operands
    Preselected = selectMemoryOperands(InstrDesc, BaseReg, AI);
    selectConcreteOffsets(InstrGenCtx, InstrDesc, Preselected);
    // Now select other operands taking into account registers we already
    // reserved as memory operands
    selectNonMemoryOperands(InstrDesc, Preselected, InstrGenCtx, RP,
                            /*Excluded=*/{}, Destinations);
    if ([[maybe_unused]] auto NumDefs = InstrDesc.getNumDefs()) {
      assert(NumDefs == 1 && "Multiple destination operands are not supported");
      assert(Preselected[0].isReg());
      SmallVector<Register, 8> Dsts;
      Tgt.getPhysRegsFromUnit(Preselected[0].getReg(), RegInfo, Dsts);
      Destinations.insert_range(Dsts);
    }

    // All registers are selected now. Break down the address into parts to
    // initialize properly.
    // The only registers with known values to initialize here are address
    // registers. So we can safely call getZextValue() on their APInt.
    assert(RegsToInit[BaseReg].getBitWidth() <=
           Tgt.getAddrRegLen(State.getTargetMachine()));
    auto &&[RegToValue, ChosenAddresses] =
        Tgt.breakDownAddr(InstrGenCtx, AI, InstrDesc, Preselected, 0,
                          RegsToInit[BaseReg].getZExtValue());
    for (auto &AP : RegToValue) {
      auto &Reg = AP.FixedReg;
      auto &Val = AP.Value;
      RP.addReserved(Reg, AccessMaskBit::SupportW);
      RegsToInit[Reg] = Val;
    }
    AddressInfo ActualAI = AI;
    auto Offset = getOffsetImmediate(Preselected);
    ActualAI.Address += Offset.value_or(0);
    markMemAccessAsUsed(InstrGenCtx, InstrDesc, ActualAI, MemAccessKind::BURST,
                        InstrGenCtx.MAI);
  }
  return {OpcodeIdxToPreselectedOps, RegsToInit};
}

void selectConcreteOffsets(
    InstructionGenerationContext &IGC, const MCInstrDesc &InstrDesc,
    SmallVectorImpl<planning::PreselectedOpInfo> &Preselected) {
  auto MappedRange = map_range(
      enumerate(Preselected), [&](auto &&Args) -> planning::PreselectedOpInfo {
        auto &[Idx, Operand] = Args;
        if (Operand.isImm()) {
          auto &ProgCtx = IGC.ProgCtx;
          auto &Cfg = IGC.getCommonCfg();
          auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
          auto Concrete = Tgt.generateTargetOperand(
              InstrDesc, Idx, Operand.getImm(), ProgCtx, Cfg);
          return StridedImmediate(Concrete.getImm(), Concrete.getImm(),
                                  Operand.getImm().getStride());
        }
        return Operand;
      });
  copy(MappedRange, Preselected.begin());
}

// Memory schemes return random address with such offsets that they include zero
// offset. So, when memory scheme is restrictive, for example has small size, we
// can generate only small immediate offsets. For example for the scheme below
//
//   access-ranges:
//      - start: 0x800FFFF0
//        size: 0x10
//        stride: 1
//        first-offset: 0
//        last-offset: 0
//
// Memory scheme may return the following address infos:
//
//  Base Addr    MinOff  MaxOff
// [0x800FFFF0,    0,     15]
// [0x800FFFF1,   -1,     14]
// [0x800FFFF2,   -2,     13]
// ...
// [0x800FFFFE,   -14,     1]
// [0x800FFFFF,   -15,     0]
//
// Such offsets may not cover whole possible range of ImmOffset field in
// instruction encoding (RISC-V in general allows 12-bit signed immediate).
//
// To make randomization better we generate a random shift for the offsets such
// that they will cover a part of imm range but not necessary contain zero
// offset. For example,
//   AddressInfo holds [0x800FFFF2, -2, 13],
//   immediate field allows immediates [-2048, 2047].
// Then we calculate random shift in range
//   [-2048 - (-2), 2047 - 13] => [-2046, 2034].
// Let's say randomly chosen value is -1000, we apply it to original range from
// address info:
//   [0x800FFFF2, -2, 13] => [0x800FFFF2 - (-1000), -2 + (-1000), 13 + (-1000)]
//     => [0x801003DA, -1002, -987].
// As you can check, all possible addresses (base addr + offset) match for both
// ranges.
//
// If the legal values for an immediate aren't an interval (which is the case
// for RISC-V compressed loads and stores), the random shift must be generated
// with some kind of stride.

AddressInfo randomlyShiftAddressOffsetsInImmRange(AddressInfo AI,
                                                  StridedImmediate ImmRange,
                                                  unsigned OffsetAlignment) {
  auto MinImm = ImmRange.getMin();
  auto MaxImm = ImmRange.getMax();
  assert(MinImm <= 0 && 0 <= MaxImm);
  assert(ImmRange.getStride() == 0 || MinImm % ImmRange.getStride() == 0);
  assert(AI.MinOffset <= 0 && 0 <= AI.MaxOffset);

  assert(ImmRange.getStride() <= std::numeric_limits<int64_t>::max() &&
         "Unexpected stride for immediate");
  assert(AI.MinStride <= std::numeric_limits<int64_t>::max() &&
         "Unexpected stride for AI");
  auto Stride = std::max<int64_t>(ImmRange.getStride(), AI.MinStride);
  Stride = std::max<int64_t>(Stride, 1);
  auto LCStride = std::lcm<int64_t, int64_t>(Stride, OffsetAlignment);
  // Address info might be less restrictive than the immediate operand. For
  // example, legal final address can be aligned to 4, but the immediate operand
  // must be aligned to 8. So, when choosing legal immediate range, we must
  // account such restrictions.
  AI.MinOffset = alignTo(AI.MinOffset, LCStride);
  assert(AI.MinOffset % LCStride == 0);
  AI.MaxOffset = alignDown(AI.MaxOffset, LCStride);
  assert(AI.MaxOffset % LCStride == 0);
  assert(AI.MinOffset <= 0 && 0 <= AI.MaxOffset);

  if (!(AI.MinOffset < MinImm && MaxImm < AI.MaxOffset)) {
    auto Shift = LCStride *
                 RandEngine::genInRangeInclusive<int64_t>(
                     std::min<int64_t>((MinImm - AI.MinOffset) / LCStride, 0),
                     std::max<int64_t>((MaxImm - AI.MaxOffset) / LCStride, 0));
    AI.MinOffset += Shift;
    AI.MaxOffset += Shift;
    AI.Address -= Shift;
  }

  AI.MinStride = LCStride;
  assert(AI.MinOffset % LCStride == 0);
  assert(AI.MaxOffset % LCStride == 0);
  return AI;
}

std::map<unsigned, AddressRestriction> deduceStrongestRestrictions(
    ArrayRef<unsigned> Opcodes, ArrayRef<unsigned> OpcodeIdxToBaseReg,
    const std::map<unsigned, AddressRestriction> &OpcodeToAR) {
  assert(Opcodes.size() == OpcodeIdxToBaseReg.size());
  assert(all_of(Opcodes, [&OpcodeToAR](auto Opcode) {
    return OpcodeToAR.count(Opcode);
  }));
  std::map<unsigned, std::set<unsigned>> BaseRegToOpcodes;
  for (auto [OpcodeIdx, BaseReg] : enumerate(OpcodeIdxToBaseReg))
    BaseRegToOpcodes[BaseReg].insert(Opcodes[OpcodeIdx]);

  std::map<unsigned, AddressRestriction> BaseRegToAR;
  for (const auto &[BaseReg, Opcodes] : BaseRegToOpcodes) {
    auto ARsRange =
        map_range(Opcodes, [&](unsigned Opc) { return OpcodeToAR.at(Opc); });
    SmallVector<AddressRestriction, 8> ARs(ARsRange.begin(), ARsRange.end());

#define SNIPPY_ARS_GET_MAX_FIELD(FIELD, COMPARE)                               \
  std::max_element(ARs.begin(), ARs.end(),                                     \
                   [](const auto &LHS, const auto &RHS) {                      \
                     return COMPARE(LHS.FIELD, RHS.FIELD);                     \
                   })                                                          \
      ->FIELD

    BaseRegToAR[BaseReg] = AddressRestriction{
        // Max size
        SNIPPY_ARS_GET_MAX_FIELD(AccessSize, std::less<>{}),
        // Max alignment
        SNIPPY_ARS_GET_MAX_FIELD(AccessAlignment, std::less<>{}),
        // Max alignment
        SNIPPY_ARS_GET_MAX_FIELD(OffsetAlignment, std::less<>{}),
        // AllowMisalign only if all ARs allow it
        SNIPPY_ARS_GET_MAX_FIELD(AllowMisalign, std::greater<>{}),
        {
            // Largest min
            SNIPPY_ARS_GET_MAX_FIELD(ImmOffsetRange.getMin(), std::less<>{}),
            // Smallest max
            SNIPPY_ARS_GET_MAX_FIELD(ImmOffsetRange.getMax(), std::greater<>{}),
            // Max stride
            SNIPPY_ARS_GET_MAX_FIELD(ImmOffsetRange.getStride(), std::less<>{}),
        },
        // We insert all opcodes because the address chosen for this restriction
        // will be used as a fallback if we fail to find another one.
        decltype(AddressRestriction::Opcodes)(Opcodes.begin(), Opcodes.end())};
#undef SNIPPY_ARS_GET_MAX_FIELD
  }

  return BaseRegToAR;
}

std::map<unsigned, AddressRestriction>
collectAddressRestrictions(ArrayRef<unsigned> Opcodes,
                           SnippyProgramContext &ProgCtx,
                           const MachineBasicBlock &MBB) {
  std::map<unsigned, AddressRestriction> OpcodeToAR;
  const auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  for (auto Opcode : Opcodes) {
    const auto &InstrDesc = InstrInfo.get(Opcode);
    if (!SnippyTgt.canUseInBurstMode(InstrDesc))
      continue;

    const auto AddrGenInfo =
        SnippyTgt.selectAddrGenInfoForInstr(ProgCtx, Opcode, MBB);
    // Get address restrictions for the current opcode.
    AddressRestriction AR{
        AddrGenInfo.AccessSize,
        AddrGenInfo.Alignment,
        SnippyTgt.getImmOffsetAlignmentForMemAccessInst(InstrDesc),
        AddrGenInfo.AllowMisalign,
        SnippyTgt.getImmOffsetRangeForMemAccessInst(InstrDesc),
        /*Opcodes=*/{Opcode},
    };

    assert(!OpcodeToAR.count(Opcode) ||
           OpcodeToAR[Opcode].ImmOffsetRange == AR.ImmOffsetRange);
    OpcodeToAR.try_emplace(Opcode, AR);
  }
  return OpcodeToAR;
}

using BRGroupRefTy =
    std::reference_wrapper<const BurstGramData::UniqueOpcodesTy>;
static BRGroupRefTy
selectBRGroupWithOpcode(const BurstGramData::OpcodeGroupsTy &BaseRegisterGroups,
                        unsigned Opcode) {
  auto DoesNotContainOpcode = [&Opcode](auto GroupRef) {
    return !GroupRef.count(Opcode);
  };
  auto ExpectedGroup = RandEngine::selectFromContainerFiltered(
      BaseRegisterGroups, DoesNotContainOpcode);
  assert(ExpectedGroup && "Each opcode must appear in at least one group");
  return std::cref(*ExpectedGroup);
}

static unsigned
selectBaseRegForOpcode(unsigned Opcode,
                       std::unordered_map<unsigned, BRGroupRefTy> &RegToBRGroup,
                       ArrayRef<unsigned> AvailableRegs) {
  // A register is allowed for an opcode if it is bound to a base‑register
  // group that contains the opcode, or if it is not bound to any group.
  auto IsAllowed = [&RegToBRGroup, &Opcode](auto Reg) -> bool {
    if (!RegToBRGroup.count(Reg))
      return true;
    auto GroupRef = RegToBRGroup.find(Reg)->second;
    return GroupRef.get().count(Opcode);
  };

  auto ExpectedReg = RandEngine::selectFromContainerFiltered(
      AvailableRegs, std::not_fn(IsAllowed));
  if (!ExpectedReg)
    snippy::fatal("Failed to select base register for burst group",
                  formatv("No available registers for {0} opcode", Opcode));
  return *ExpectedReg;
}

static std::vector<unsigned>
generateBaseRegsForOpcodes(InstructionGenerationContext &IGC,
                           ArrayRef<unsigned> Opcodes,
                           ArrayRef<unsigned> AvailableRegs) {
  std::vector<unsigned> Res(Opcodes.size());

  if (!IGC.hasCfg<BurstPolicyConfig>() ||
      !IGC.getCfg<BurstPolicyConfig>().Burst.BaseRegisterGroups) {
    std::generate(Res.begin(), Res.end(), [&AvailableRegs]() {
      return RandEngine::selectFromContainer(AvailableRegs);
    });
    return Res;
  }

  const auto &BaseRegsGroups =
      *IGC.getCfg<BurstPolicyConfig>().Burst.BaseRegisterGroups;
  std::unordered_map<unsigned, BRGroupRefTy> RegToBRGroup;
  transform(Opcodes, Res.begin(), [&](auto Opcode) {
    auto Reg = selectBaseRegForOpcode(Opcode, RegToBRGroup, AvailableRegs);
    // Bind the selected register to some base-register-group that
    // contains this opcode
    if (!RegToBRGroup.count(Reg))
      RegToBRGroup.emplace(Reg,
                           selectBRGroupWithOpcode(BaseRegsGroups, Opcode));
    return Reg;
  });
  return Res;
}

std::vector<unsigned>
generateBaseRegs(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> Opcodes) {
  if (Opcodes.empty())
    return {};
  auto &MBB = InstrGenCtx.MBB;
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &Cfg = InstrGenCtx.getCommonCfg();
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  const auto &RI = State.getRegInfo();
  // Compute set of registers compatible with all opcodes
  std::unordered_set<unsigned> Exclude;
  for (auto Opcode : Opcodes) {
    SmallVector<Register> ExcludedRegs;
    SnippyTgt.excludeFromMemRegsForInstr(InstrInfo.get(Opcode), RI,
                                         ExcludedRegs);
    copy(ExcludedRegs, std::inserter(Exclude, Exclude.begin()));
  }
  // Current implementation expects that each target has only one addr reg
  // class.
  const auto &AddrRegClass = SnippyTgt.getAddrRegClass();
  SmallVector<Register, 32> Include;
  copy_if(AddrRegClass, std::back_inserter(Include), [&](Register Reg) {
    SmallVector<Register> Units;
    SnippyTgt.getPhysRegsFromUnit(Reg, RI, Units);
    return none_of(Units, [&Exclude](auto R) { return Exclude.count(R); });
  });

  // Normalize the number of addr registers to use. It's possible that we'll
  // re-use addr regs with different offset values.
  // FIXME: normalization does not account restrictions from memory schemes:
  // choosen number of base registers might not be enough.
  auto NumAvailRegs = RP.getNumAvailableInSet(Include, MBB);
  if (NumAvailRegs > 0 && Cfg.TrackCfg.AddressVH) {
    // When hazard mode is enabled we'll likely need a register to transform
    // existing addresses.
    --NumAvailRegs;
  }
  if (NumAvailRegs == 0)
    snippy::fatal(
        "No available registers to generate addresses for the burst group.");
  const auto *MF = InstrGenCtx.MBB.getParent();
  const auto &RegInfo = *MF->getSubtarget().getRegisterInfo();
  // Get number of def and addr regs to use in the burst group. These values
  // can be bigger than the number of available registers.
  auto NumDefs = countDefsHavingRC(Opcodes, RegInfo, AddrRegClass, InstrInfo);
  auto NumAddrs = countAddrs(Opcodes, SnippyTgt);
  // Count the minimum number of available registers we need.
  unsigned MinAvailRegs = 0;
  // If there is one address or more then we need at least one register
  // available for it.
  if (NumAddrs > 0)
    ++MinAvailRegs;
  // Same for definitions. If there are some definitions then we need at least
  // one register available for it.
  if (NumDefs > 0)
    ++MinAvailRegs;
  if (MinAvailRegs > NumAvailRegs)
    snippy::fatal(
        "Cannot generate burst group: don't have enough registers available. "
        "Please, try to reduce amount of registers reserved by decreasing "
        "loops nestness or change instructions used in burst groups if these "
        "instructions may be used with a very limited set of registers.");

  NumAddrs = normalizeNumRegs(NumDefs, NumAddrs, NumAvailRegs);

  // Randomly pick and reserve addr registers so as not to use them
  // destinations.
  auto AddrRegs = RP.getNAvailableRegisters(
      "for memory access burst", RegInfo, *AddrRegClass.MC, MBB, NumAddrs,
      [&](Register R) {
        SmallVector<Register> Units;
        SnippyTgt.getPhysRegsFromUnit(R, RI, Units);
        return any_of(Units,
                      [&Exclude](auto Reg) { return Exclude.count(Reg); });
      });

  for (auto Reg : AddrRegs)
    RP.addReserved(Reg, AccessMaskBit::W);

  // We must be sure that each memory access in the burst group won't contradict
  // given memory schemes. To do that we 'attach' base address register from the
  // chosen above to each opcode (`OpcodeIdxToBaseReg`). After that we will know
  // a group of opcodes for each base register. Then for each group of opcodes
  // we collect restrictions on memory addresses such as stride and access size
  // (`BaseRegToAI` as we've already had a mapping from opcode index to the base
  // register). Gathered information gives us restriction on the offset
  // immediate for each base register for each opcode.
  return generateBaseRegsForOpcodes(InstrGenCtx, Opcodes, AddrRegs);
}

// Collect addresses that will meet the specified restrictions. We call such
// addresses "primary" because they'll be used as a defaults for the given base
// registers (set of opcodes mapped to the base register). Snippy will try to
// randomize addresses in a way that not only primary addresses are accessed
// (see selectAddressForSingleInstrFromBurstGroup), but base register is always
// taken suitable for the primary address.
static std::map<unsigned, AddressInfo> collectPrimaryAddresses(
    InstructionGenerationContext &IGC,
    const std::map<unsigned, AddressRestriction> &BaseRegToStrongestAR) {
  auto &MS = IGC.getMemoryAccessSampler();
  auto &ProgCtx = IGC.ProgCtx;
  auto &SnpTgt = ProgCtx.getLLVMState().getSnippyTarget();
  auto ARRange = make_second_range(BaseRegToStrongestAR);
  std::vector<AddressRestriction> ARs(ARRange.begin(), ARRange.end());
  std::vector<AddressInfo> PrimaryAddresses =
      MS.randomBurstGroupAddresses(ARs, ProgCtx.getOpcodeCache(), SnpTgt);
  assert(PrimaryAddresses.size() == BaseRegToStrongestAR.size());
  std::map<unsigned, AddressInfo> BaseRegToPrimaryAddress;
  transform(
      zip(make_first_range(BaseRegToStrongestAR), std::move(PrimaryAddresses)),
      std::inserter(BaseRegToPrimaryAddress, BaseRegToPrimaryAddress.begin()),
      [](auto BaseRegToAI) {
        auto &&[BaseReg, AI] = BaseRegToAI;
        return std::make_pair(BaseReg, std::move(AI));
      });

  // Do additional randomization of immediate offsets for each address info to
  // have a uniform distribution imm offsets (otherwise, for the majority of
  // real memory schemes they'll be around zero).
  assert(BaseRegToStrongestAR.size() == BaseRegToPrimaryAddress.size());
  for (auto &&[RegToAR, RegToAI] :
       zip(BaseRegToStrongestAR, BaseRegToPrimaryAddress)) {
    auto &[Reg, AR] = RegToAR;
    auto &[BaseReg, AI] = RegToAI;
    assert(BaseReg == Reg);
    (void)BaseReg, (void)Reg;
    AI = randomlyShiftAddressOffsetsInImmRange(AI, AR.ImmOffsetRange,
                                               AR.OffsetAlignment);
  }
  return BaseRegToPrimaryAddress;
}

// Insert initialization of base addresses before the burst group.
void initializeBaseRegs(InstructionGenerationContext &InstrGenCtx,
                        const std::map<unsigned, APInt> &BaseRegToValue) {
  auto &SimCtx = InstrGenCtx.SimCtx;
  [[maybe_unused]] auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  for (auto &[BaseReg, NewValue] : BaseRegToValue) {
    assert(RP.isReserved(BaseReg, AccessMaskBit::W));
    if (InstrGenCtx.getCommonCfg().TrackCfg.AddressVH) {
      auto &I = SimCtx.getInterpreter();
      auto OldValue = I.readReg(BaseReg);
      SnippyTgt.transformValueInReg(InstrGenCtx, OldValue, NewValue, BaseReg);
    } else
      SnippyTgt.writeValueToReg(InstrGenCtx, NewValue, BaseReg);
  }
}

// TODO: that should not be here.
void markMemAccess(InstructionGenerationContext &IGC,
                   const MemAddresses &Addresses, size_t AccessSize,
                   const MCInstrDesc &InstrDesc) {
  if (!IGC.getCommonCfg()
           .ProgramCfg.MemoryCfg.InitializationMode.isLoadsInit() ||
      !InstrDesc.mayLoad())
    return;
  IGC.ProgCtx.getMemoryManager().markMemAccessToInitialize(Addresses,
                                                           AccessSize);
}

// This function returns address info to use for each opcode.
std::pair<std::map<unsigned, APInt>, std::vector<AddressInfo>>
mapOpcodeIdxToAI(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes) {
  assert(OpcodeIdxToBaseReg.size() == Opcodes.size());
  auto &MBB = InstrGenCtx.MBB;
  auto *MAI = InstrGenCtx.MAI;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
  if (Opcodes.empty())
    return {};

  // FIXME: This code does not account non-trivial cases when an opcode
  // (vluxseg<nf>ei<eew>.v, vssseg<nf>e<eew>.v, etc) has
  // additional restrictions on the address register (e.g.
  // `RISCVGeneratorContext::getEMUL`).

  std::vector<AddressInfo> OpcodeIdxToAI;
  // Collect address restrictions for each opcode
  auto OpcodeToAR = collectAddressRestrictions(Opcodes, ProgCtx, MBB);
  // For each base register we have a set of opcodes. Join address restrictions
  // for these set of opcodes by choosing the strongest ones and map the
  // resulting address restriction to the base register.
  auto BaseRegToStrongestAR =
      deduceStrongestRestrictions(Opcodes, OpcodeIdxToBaseReg, OpcodeToAR);
  // For the selected strongest restrictions get addresses. Thus, we'll have a
  // mapping from base register to a legal address in memory to use.
  auto BaseRegToPrimaryAddress =
      collectPrimaryAddresses(InstrGenCtx, BaseRegToStrongestAR);

  // Try to find addresses for each opcode that allow better randomization of
  // offsets and effective addresses. If no address is found, we can always use
  // the primary one for the given base reg.
  for (auto [OpcodeIdx, Opcode] : enumerate(Opcodes)) {
    assert(OpcodeToAR.count(Opcode));
    assert(OpcodeIdxToBaseReg.size() > OpcodeIdx);
    const auto &OpcodeAR = OpcodeToAR[Opcode];
    auto BaseReg = OpcodeIdxToBaseReg[OpcodeIdx];
    assert(BaseRegToPrimaryAddress.count(BaseReg));
    const auto &OrigAI = BaseRegToPrimaryAddress[BaseReg];
    auto AI = selectAddressForSingleInstrFromBurstGroup(InstrGenCtx, OrigAI,
                                                        OpcodeAR);
    OpcodeIdxToAI.push_back(AI);
  }

  if (MAI)
    MAI->addBurstRangeMemAccess(OpcodeIdxToAI);
  std::map<unsigned, APInt> BaseRegToAddr;
  transform(BaseRegToPrimaryAddress,
            std::inserter(BaseRegToAddr, BaseRegToAddr.end()),
            [&](auto &RegAndAI) {
              auto &&[Reg, AI] = RegAndAI;
              return std::make_pair(
                  Reg, APInt(Tgt.getRegBitWidth(Reg, InstrGenCtx), AI.Address));
            });

  return {std::move(BaseRegToAddr), std::move(OpcodeIdxToAI)};
}

void markMemAccessAsUsed(InstructionGenerationContext &IGC,
                         const MCInstrDesc &InstrDesc, const AddressInfo &AI,
                         MemAccessKind Kind, MemAccessInfo *MAI) {
  auto EffectiveAddr = AI.Address;
  auto AccessSize = AI.AccessSize;
  auto AddrToAccess = MemAddresses{EffectiveAddr};
  markMemAccess(IGC, AddrToAccess, AccessSize, InstrDesc);
  if (MAI) {
    if (Kind == MemAccessKind::BURST)
      MAI->addBurstPlainMemAccess(EffectiveAddr, AccessSize);
    else
      MAI->addMemAccess(EffectiveAddr, AccessSize);
  }
}

void addMemAccessToDump(const MemAddresses &ChosenAddresses, MemAccessInfo &MAI,
                        size_t AccessSize) {
  for (auto Addr : ChosenAddresses)
    MAI.addMemAccess(Addr, AccessSize);
}

MachineBasicBlock *createMachineBasicBlock(MachineFunction &MF) {
  auto *MBB = MF.CreateMachineBasicBlock();
  assert(MBB);
  return MBB;
}

std::string getMBBSectionName(const MachineBasicBlock &MBB) {
  auto *MF = MBB.getParent();
  assert(MF);
  auto FunctionSectionName = MF->getFunction().getSection();
  auto *Symb = MBB.getSymbol();
  assert(Symb);
  auto ret =
      llvm::formatv("{0}.{1}", FunctionSectionName, Symb->getName()).str();
  return ret;
}

GlobalVariable *getGVForMBB(const MachineBasicBlock &MBB, GlobalsPool &GP,
                            SnippyProgramContext &ProgCtx) {
  if (MBB.getParent()->getName() == SMCManagerT::SMCSrcFuncName)
    return ProgCtx.getSMCManager().getGVFromSMCSrcMap(&MBB);

  auto &State = ProgCtx.getLLVMState();
  auto &Tgt = State.getSnippyTarget();

  auto AddrLen = Tgt.getAddrRegLen(State.getTargetMachine());

  auto ToName = getMBBSectionName(MBB);
  auto *GV = GP.getGV(ToName);
  if (!GV)
    GV = GP.createGV(APInt::getZero(AddrLen), /*Alignment*/ 1,
                     GlobalValue::ExternalLinkage, ToName,
                     /*Reason*/ "Relocation for BB address",
                     /* IsConst */ true);
  return GV;
}

} // namespace snippy
} // namespace llvm
