//===-- GenerationUtils.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationUtils.h"
#include "snippy/Generator/MemAccessInfo.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Generator/SimulatorContext.h"
#include "snippy/Support/Options.h"
#include "llvm/CodeGen/MachineLoopInfo.h"

#include "llvm/Support/Debug.h"

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
    auto CandidateAccess =
        MS.sample(OpcodeAR.AccessSize, OpcodeAR.AccessAlignment, false);
    if (auto Err = CandidateAccess.takeError()) {
      std::string PrefixErr;
      raw_string_ostream OS(PrefixErr);
      OS << "Cannot sample memory access for single instruction from burst "
            "group";
      snippy::fatal(PrefixErr, toString(std::move(Err)));
    }
    auto &CandidateAI = CandidateAccess->AddrInfo;

    auto Stride = std::max<int64_t>(OpcodeAR.ImmOffsetRange.getStride(),
                                    CandidateAI.MinStride);
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

static Register pregenerateRegister(InstructionGenerationContext &InstrGenCtx,
                                    const MCInstrDesc &InstrDesc,
                                    const MCOperandInfo &MCOpInfo,
                                    unsigned OpIndex) {
  auto &MBB = InstrGenCtx.MBB;
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  const auto &State = ProgCtx.getLLVMState();
  const auto &RegInfo = State.getRegInfo();
  const auto &Tgt = State.getSnippyTarget();
  auto OperandRegClassID = InstrDesc.operands()[OpIndex].RegClass;
  auto RegClass = Tgt.getRegClass(InstrGenCtx, OperandRegClassID, OpIndex,
                                  InstrDesc.getOpcode(), RegInfo);
  auto Exclude =
      Tgt.excludeRegsForOperand(InstrGenCtx, RegClass, InstrDesc, OpIndex);
  auto Include = Tgt.includeRegs(RegClass);
  AccessMaskBit Mask = AccessMaskBit::RW;
  auto CustomMask = Tgt.getCustomAccessMaskForOperand(InstrDesc, OpIndex);
  if (CustomMask != AccessMaskBit::None)
    Mask = CustomMask;
  auto RegOpt =
      ProgCtx.getRegGen().generate(RegClass, OperandRegClassID, RegInfo, RP,
                                   MBB, Tgt, Exclude, Include, Mask);
  assert(RegOpt.has_value());
  return RegOpt.value();
}

std::vector<planning::PreselectedOpInfo>
selectInitializableOperandsRegisters(InstructionGenerationContext &InstrGenCtx,
                                     const MCInstrDesc &InstrDesc) {
  std::vector<planning::PreselectedOpInfo> Preselected;
  Preselected.reserve(InstrDesc.getNumOperands());
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  llvm::transform(
      llvm::enumerate(InstrDesc.operands()), std::back_inserter(Preselected),
      [&, &Tgt = ProgCtx.getLLVMState().getSnippyTarget()](
          const auto &&Args) -> planning::PreselectedOpInfo {
        const auto &[OpIndex, MCOpInfo] = Args;
        // If it is TIED_TO, this register is already selected.
        if (Tgt.canInitializeOperand(InstrDesc, OpIndex) &&
            InstrDesc.getOperandConstraint(OpIndex, MCOI::TIED_TO) == -1)
          return pregenerateRegister(InstrGenCtx, InstrDesc, MCOpInfo, OpIndex);
        return {};
      });
  return Preselected;
}

static planning::PreselectedOpInfo
convertToPreselectedOpInfo(const MCOperand &Op) {
  if (Op.isReg())
    return Register(Op.getReg());
  if (Op.isImm())
    return StridedImmediate(/* MinIn */ Op.getImm(), /* MaxIn */ Op.getImm(),
                            /* StrideIn */ 0);
  llvm_unreachable("Unknown MCOperand");
}

std::vector<planning::PreselectedOpInfo>
getPreselectedForInstr(const MCInst &Inst) {
  std::vector<planning::PreselectedOpInfo> Preselected;
  Preselected.reserve(Inst.getNumOperands());

  transform(Inst, std::back_inserter(Preselected), [](const auto &Operand) {
    return convertToPreselectedOpInfo(Operand);
  });
  return Preselected;
}

// For the given InstrDesc fill the vector of selected operands to account them
// in instruction generation procedure.
std::vector<planning::PreselectedOpInfo>
selectOperands(const MCInstrDesc &InstrDesc, unsigned BaseReg,
               const AddressInfo &AI) {
  std::vector<planning::PreselectedOpInfo> Preselected;
  for (const auto &MCOpInfo : InstrDesc.operands()) {
    if (MCOpInfo.OperandType == MCOI::OperandType::OPERAND_MEMORY)
      Preselected.emplace_back(BaseReg);
    else if (MCOpInfo.OperandType >= MCOI::OperandType::OPERAND_FIRST_TARGET) {
      // FIXME: Here we just use the fact that RISC-V loads and stores from base
      // subset have only one target specific operand that is offset.
      auto MinStride = AI.MinStride;
      if (MinStride == 0)
        MinStride = 1;
      assert(isPowerOf2_64(MinStride));
      Preselected.emplace_back(
          StridedImmediate(AI.MinOffset, AI.MaxOffset, MinStride));
    } else
      Preselected.emplace_back();
  }
  return Preselected;
}

std::vector<planning::PreselectedOpInfo> selectConcreteOffsets(
    InstructionGenerationContext &IGC, const MCInstrDesc &InstrDesc,
    const std::vector<planning::PreselectedOpInfo> &Preselected) {
  auto MappedRange = map_range(
      enumerate(Preselected), [&](auto &&Args) -> planning::PreselectedOpInfo {
        auto &[Idx, Operand] = Args;
        if (Operand.isImm()) {
          auto OpType = InstrDesc.operands()[Idx].OperandType;
          auto &ProgCtx = IGC.ProgCtx;
          auto &GenSettings = IGC.GenSettings;
          auto &Tgt = ProgCtx.getLLVMState().getSnippyTarget();
          auto Concrete = Tgt.generateTargetOperand(ProgCtx, GenSettings,
                                                    InstrDesc.getOpcode(),
                                                    OpType, Operand.getImm());
          return StridedImmediate(Concrete.getImm(), Concrete.getImm(),
                                  Operand.getImm().getStride());
        }
        return Operand;
      });
  return std::vector<planning::PreselectedOpInfo>(MappedRange.begin(),
                                                  MappedRange.end());
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
                                                  StridedImmediate ImmRange) {
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

  // Address info might be less restrictive than the immediate operand. For
  // example, legal final address can be aligned to 4, but the immediate operand
  // must be aligned to 8. So, when choosing legal immediate range, we must
  // account such restrictions.
  AI.MinOffset = alignTo(AI.MinOffset, Stride);
  AI.MaxOffset = alignDown(AI.MaxOffset, Stride);
  assert(AI.MinOffset <= 0 && 0 <= AI.MaxOffset);

  if (!(AI.MinOffset < MinImm && MaxImm < AI.MaxOffset)) {
    auto Shift =
        Stride * RandEngine::genInInterval<int64_t>(
                     std::min<int64_t>((MinImm - AI.MinOffset) / Stride, 0),
                     std::max<int64_t>((MaxImm - AI.MaxOffset) / Stride, 0));
    AI.MinOffset += Shift;
    AI.MaxOffset += Shift;
    AI.Address -= Shift;
  }

  AI.MinStride = Stride;
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
    auto StrongestAccessSize = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).AccessSize < OpcodeToAR.at(RHS).AccessSize;
        });
    auto StrongestImmMin = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getMin() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getMin();
        });
    auto StrongestImmMax = std::min_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getMax() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getMax();
        });
    auto StrongestImmStride = std::max_element(
        Opcodes.begin(), Opcodes.end(), [&OpcodeToAR](auto LHS, auto RHS) {
          return OpcodeToAR.at(LHS).ImmOffsetRange.getStride() <
                 OpcodeToAR.at(RHS).ImmOffsetRange.getStride();
        });

    AddressRestriction StrongestAR;

    StrongestAR.AccessSize = OpcodeToAR.at(*StrongestAccessSize).AccessSize;
    StrongestAR.AccessAlignment =
        OpcodeToAR.at(*StrongestAccessSize).AccessAlignment;

    StrongestAR.ImmOffsetRange = StridedImmediate(
        OpcodeToAR.at(*StrongestImmMin).ImmOffsetRange.getMin(),
        OpcodeToAR.at(*StrongestImmMax).ImmOffsetRange.getMax(),
        OpcodeToAR.at(*StrongestImmStride).ImmOffsetRange.getStride());

    // We insert all opcodes because the address chosen for this restriction
    // will be used as a fallback if we fail to find another one.
    StrongestAR.Opcodes.insert(Opcodes.begin(), Opcodes.end());

    BaseRegToAR[BaseReg] = std::move(StrongestAR);
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
    if (!SnippyTgt.canUseInMemoryBurstMode(Opcode))
      continue;

    // Get address restrictions for the current opcode.
    AddressRestriction AR;
    AR.Opcodes.insert(Opcode);
    std::tie(AR.AccessSize, AR.AccessAlignment) =
        SnippyTgt.getAccessSizeAndAlignment(ProgCtx, Opcode, MBB);
    AR.ImmOffsetRange = SnippyTgt.getImmOffsetRangeForMemAccessInst(InstrDesc);

    assert(!OpcodeToAR.count(Opcode) ||
           OpcodeToAR[Opcode].ImmOffsetRange == AR.ImmOffsetRange);
    OpcodeToAR.try_emplace(Opcode, AR);
  }
  return OpcodeToAR;
}

std::vector<unsigned>
generateBaseRegs(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> Opcodes) {
  if (Opcodes.empty())
    return {};
  auto &MBB = InstrGenCtx.MBB;
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &GenSettings = InstrGenCtx.GenSettings;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  const auto &InstrInfo = State.getInstrInfo();
  // Compute set of registers compatible with all opcodes
  std::unordered_set<unsigned> Exclude;
  for (auto Opcode : Opcodes) {
    copy(SnippyTgt.excludeFromMemRegsForOpcode(Opcode),
         std::inserter(Exclude, Exclude.begin()));
  }
  // Current implementation expects that each target has only one addr reg
  // class.
  const auto &AddrRegClass = SnippyTgt.getAddrRegClass();
  SmallVector<Register, 32> Include;
  copy_if(AddrRegClass, std::back_inserter(Include),
          [&](Register Reg) { return !Exclude.count(Reg); });

  // Normalize the number of addr registers to use. It's possible that we'll
  // re-use addr regs with different offset values.
  // FIXME: normalization does not account restrictions from memory schemes:
  // choosen number of base registers might not be enough.
  auto NumAvailRegs = RP.getNumAvailableInSet(Include, MBB);
  if (NumAvailRegs > 0 && GenSettings.TrackingConfig.AddressVH) {
    // When hazard mode is enabled we'll likely need a register to transform
    // existing addresses.
    --NumAvailRegs;
  }
  if (NumAvailRegs == 0)
    snippy::fatal(
        "No available registers to generate addresses for the burst group.");
  auto &Fn = InstrGenCtx.MBB.getParent()->getFunction();
  const auto &RegInfo = *State.getSubtargetImpl(Fn).getRegisterInfo();
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
      [&](Register R) { return Exclude.count(R); });

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
  std::vector<unsigned> OpcodeIdxToBaseReg(Opcodes.size());
  std::generate(OpcodeIdxToBaseReg.begin(), OpcodeIdxToBaseReg.end(),
                [&AddrRegs]() {
                  auto N = RandEngine::genInRange(AddrRegs.size());
                  return AddrRegs[N];
                });
  return OpcodeIdxToBaseReg;
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
    AI = randomlyShiftAddressOffsetsInImmRange(AI, AR.ImmOffsetRange);
  }
  return BaseRegToPrimaryAddress;
}

// Insert initialization of base addresses before the burst group.
void initializeBaseRegs(
    InstructionGenerationContext &InstrGenCtx,
    std::map<unsigned, AddressInfo> &BaseRegToPrimaryAddress) {
  auto &SimCtx = InstrGenCtx.SimCtx;
  auto &RP = InstrGenCtx.getRegPool();
  auto &ProgCtx = InstrGenCtx.ProgCtx;
  auto &State = ProgCtx.getLLVMState();
  const auto &SnippyTgt = State.getSnippyTarget();
  for (auto &[BaseReg, AI] : BaseRegToPrimaryAddress) {
    auto NewValue =
        APInt(SnippyTgt.getAddrRegLen(State.getTargetMachine()), AI.Address);
    assert(RP.isReserved(BaseReg, AccessMaskBit::W));
    if (SimCtx.TrackOpts.AddressVH) {
      auto &I = SimCtx.getInterpreter();
      auto OldValue = I.readReg(BaseReg);
      SnippyTgt.transformValueInReg(InstrGenCtx, OldValue, NewValue, BaseReg);
    } else
      SnippyTgt.writeValueToReg(InstrGenCtx, NewValue, BaseReg);
  }
}

// This function returns address info to use for each opcode.
std::vector<AddressInfo>
mapOpcodeIdxToAI(InstructionGenerationContext &InstrGenCtx,
                 ArrayRef<unsigned> OpcodeIdxToBaseReg,
                 ArrayRef<unsigned> Opcodes) {
  assert(OpcodeIdxToBaseReg.size() == Opcodes.size());
  auto &MBB = InstrGenCtx.MBB;
  auto *MAI = InstrGenCtx.MAI;
  auto &ProgCtx = InstrGenCtx.ProgCtx;
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
  // We've chosen addresses for each base register. Initialize base registers
  // with these addresses.
  initializeBaseRegs(InstrGenCtx, BaseRegToPrimaryAddress);

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

  return OpcodeIdxToAI;
}

void markMemAccessAsUsed(InstructionGenerationContext &IGC,
                         const MCInstrDesc &InstrDesc, const AddressInfo &AI,
                         MemAccessKind Kind, MemAccessInfo *MAI) {
  auto EffectiveAddr = AI.Address;
  auto AccessSize = AI.AccessSize;
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

} // namespace snippy
} // namespace llvm
