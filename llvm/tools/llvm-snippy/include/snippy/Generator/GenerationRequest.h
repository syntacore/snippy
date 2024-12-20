//===-- GenerationRequest.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// This file provides interfaces for generation requests used in FlowGenerator.
///
/// There are three types of generation requests:
///   * For instruction group
///   * For block
///   * For function
///
/// Function generation request contains requests for its blocks, request for
/// block contains requests for instruction groups that should be generated
/// inside it.
///
/// Generation requests workflow:
///   1. You can get generation requests for a block from corresponding function
///      request.
///   2. Block generation request consists of instruction group requests. Only
///      instruction groups expected to be generated in block.
///   3. When you generate instruction group you can get next instruction
///      request corresponding to the current generation policy and check
///      whether request is completed. After successful opcode generation you
///      should check if you need to change policy and update it in the request.
///   4. After processing all blocks request for a function, final requests must
///      be processed. They consist of generation-mode specific requests and
///      request for generation final instruction.
///
/// Note: Mixed Function Generation Request (MFGR) is special. While other types
/// of function requests contain requests of the same type, MFGR contains the
/// size and num instr generation requests for block. That's why implementation
/// for MFGR is a bit different. When mixed generation requested, it is
/// generation by number of instructions but for some blocks it's more important
/// to meet size requirements.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GenerationLimit.h"
#include "snippy/Generator/GeneratorContext.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Target/Target.h"

namespace llvm {
namespace snippy {

namespace planning {

class InstructionGroupRequest final {
  RequestLimit Limit;
  GenPolicy Policy;
  // This field is used for post-gen verification. There are some
  // instructions (like branches), size of which is not included in generation
  // request
  GenerationStatistics InitialStats;

public:
  template <typename LimitTy>
  InstructionGroupRequest(
      LimitTy ReqLimit, GenPolicy Pol,
      GenerationStatistics InitStats = GenerationStatistics{})
      : Limit(std::move(ReqLimit)), Policy(std::move(Pol)),
        InitialStats(std::move(InitStats)) {}

  std::optional<InstructionRequest> next() const {
    return planning::next(Policy);
  }

  void changePolicy(GenPolicy NewPolicy) { Policy = std::move(NewPolicy); }

  bool isLimitReached(const GenerationStatistics &Stats) const {
    return Limit.isReached(Stats);
  }

  const RequestLimit &limit() const & { return Limit; }
  auto &policy() const & { return Policy; }
  auto &policy() & { return Policy; }
  const GenerationStatistics &initialStats() const & { return InitialStats; }

  void initialize(InstructionGenerationContext &InstrGenCtx) const {
    planning::initialize(Policy, InstrGenCtx, Limit);
  }

  void finalize(InstructionGenerationContext &InstrGenCtx) const {
    planning::finalize(Policy, InstrGenCtx);
  }

  void print(raw_ostream &OS, size_t Indent = 0) const {
    OS.indent(Indent) << "InstrGroupGenerationRequest<" << Limit.getAsString()
                      << "> -- ";
    planning::print(Policy, OS);
    OS << "\n";
  }

  bool isInseparableBundle() const {
    return planning::isInseparableBundle(Policy);
  }
};

class InstrGroupGenerationRAIIWrapper final {
  const InstructionGroupRequest &IG;
  InstructionGenerationContext &InstrGenCtx;

public:
  InstrGroupGenerationRAIIWrapper(InstructionGroupRequest &Req,
                                  InstructionGenerationContext &InstrGenCtx)
      : IG(Req), InstrGenCtx(InstrGenCtx) {
    IG.initialize(InstrGenCtx);
  }

  ~InstrGroupGenerationRAIIWrapper() { IG.finalize(InstrGenCtx); }
};

namespace detail {
class InstrRequestIterator final {
  const GenPolicy &Policy;
  std::optional<InstructionRequest> CurrentReq;

public:
  InstrRequestIterator(const GenPolicy &Pol) : Policy(Pol) {
    CurrentReq = planning::next(Policy);
  }

  using value_type = InstructionRequest;
  using pointer = value_type *;
  using reference = value_type &;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::input_iterator_tag;

  operator bool() const { return CurrentReq.has_value(); }

  value_type operator*() {
    assert(CurrentReq);
    return *CurrentReq;
  }

  pointer operator->() & {
    assert(CurrentReq);
    return std::addressof(*CurrentReq);
  }

  InstrRequestIterator &operator++() {
    CurrentReq = next(Policy);
    return *this;
  }
  InstrRequestIterator operator++(int) {
    auto Tmp = *this;
    ++*this;
    return Tmp;
  }
};

class InstrRequestSentinel final {
  const GenerationStatistics &Stats;
  const RequestLimit &Limit;

public:
  InstrRequestSentinel(const GenerationStatistics &Stats,
                       const RequestLimit &Lim)
      : Stats(Stats), Limit(Lim) {}
  auto &limit() const { return Limit; }
  auto &getGenStats() const { return Stats; }
};

inline bool operator==(const InstrRequestIterator &It,
                       const InstrRequestSentinel &Sentinel) {
  return (Sentinel.limit().isReached(Sentinel.getGenStats()) || !It);
}

inline bool operator==(const InstrRequestSentinel &Sentinel,
                       const InstrRequestIterator &It) {
  return It == Sentinel;
}

inline bool operator!=(const InstrRequestIterator &It,
                       const InstrRequestSentinel &Sentinel) {
  return !(It == Sentinel);
}

inline bool operator!=(const InstrRequestSentinel &Sentinel,
                       const InstrRequestIterator &It) {
  return !(Sentinel == It);
}
} // namespace detail

class InstrRequestRange final {
  GenerationStatistics &Stats;
  const InstructionGroupRequest &Request;

public:
  InstrRequestRange(InstructionGroupRequest &Req, GenerationStatistics &Stats)
      : Stats(Stats), Request(Req) {}

  auto begin() const { return detail::InstrRequestIterator(Request.policy()); }

  auto end() const {
    return detail::InstrRequestSentinel(Stats, Request.limit());
  }
};

constexpr auto SubReqIndentSize = 2;

class BasicBlockRequest final : private std::vector<InstructionGroupRequest> {
  const MachineBasicBlock *MBB = nullptr;
  RequestLimit Limit;

public:
  BasicBlockRequest(const MachineBasicBlock &MBB)
      : MBB(&MBB), Limit(RequestLimit::NumInstrs{}) {}

  const MachineBasicBlock &getMBB() const {
    assert(MBB);
    return *MBB;
  }

  RequestLimit &limit() & { return Limit; }

  const RequestLimit &limit() const & { return Limit; }

  bool isLimitReached(const GenerationStatistics &Stats) const {
    return Limit.isReached(Stats);
  }

  using vector::begin;
  using vector::cbegin;
  using vector::cend;
  using vector::end;

  using vector::empty;
  using vector::size;

  void print(raw_ostream &OS, size_t Indent = 0) const {
    OS.indent(Indent) << "BasicBlockRequest<" << Limit.getAsString() << ">("
                      << MBB->getFullName() << ")\n";
    for_each(*this,
             [&](auto &Req) { Req.print(OS, Indent + SubReqIndentSize); });
  }

  void add(InstructionGroupRequest IG) {
    vector::emplace_back(std::move(IG));
    Limit += vector::back().limit();
  }
};

class FunctionRequest final
    : private std::map<const MachineBasicBlock *, BasicBlockRequest, MIRComp> {
  const MachineFunction *MF = nullptr;
  RequestLimit Limit;
  const MCInstrDesc *FinalInstrDesc = nullptr;
  GeneratorContext *GC = nullptr;

  void checkLimitCompatibility(const RequestLimit &Limit) const {
    assert(!(Limit.isNumLimit() && Limit.isSizeLimit()) &&
           "Num instrs generation mode for block is incompatible with "
           "function generation by size");
    assert(
        (Limit.isSizeLimit() || Limit.isNumLimit()) &&
        "Instruction group generation mode can be either num instrs or size");
    assert(!(Limit.isSizeLimit() && Limit.isNumLimit()) &&
           "Size generation mode for block is incompatible with function "
           "generation by num instrs");
  }
  // It is used in DFGenerator because sometimes we may need to change the
  // weights of opcodes.
  std::unordered_map<unsigned, double> OpcWeightOverrides;

public:
  FunctionRequest(const MachineFunction &MFn, GeneratorContext &GC,
                  const MCInstrDesc *FinalInstrDesc = nullptr)
      // FIXME: Mixed limit there to accept both SizeLimit and NumInstrsLimit.
      : MF(&MFn), Limit(RequestLimit::Mixed{}), FinalInstrDesc(FinalInstrDesc),
        GC(&GC){};

  auto &getOpcodeWeightOverrides() { return OpcWeightOverrides; }
  const auto &getOpcodeWeightOverrides() const { return OpcWeightOverrides; }

  void setFinalInstr(const MCInstrDesc *Desc) { FinalInstrDesc = Desc; }

  void addToBlock(const MachineBasicBlock *MBB, InstructionGroupRequest IG) {
    assert(MBB);
    checkLimitCompatibility(IG.limit());
    auto Found = map::find(MBB);
    if (Found == map::end()) {
      BasicBlockRequest BB(*MBB);
      BB.add(std::move(IG));
      add(MBB, std::move(BB));
    } else {
      auto &BB = Found->second;
      auto Lim = IG.limit();
      BB.add(std::move(IG));
      Limit += Lim;
    }
  }

  bool isLimitReached(const GenerationStatistics &Stats) const {
    return Limit.isReached(Stats);
  }
  void add(const MachineBasicBlock *MBB, BasicBlockRequest &&BB) {
    assert(MBB);
    checkLimitCompatibility(BB.limit());
    auto [It, WasInserted] = map::try_emplace(MBB, std::move(BB));
    assert(WasInserted);
    Limit += It->second.limit();
  }

  const RequestLimit &limit() const & { return Limit; }

  using map::begin;
  using map::cbegin;
  using map::cend;
  using map::end;

  using map::at;
  using map::clear;
  using map::count;
  using map::empty;
  using map::find;
  using map::size;

  std::vector<InstructionGroupRequest>
  getFinalGenReqs(const GenerationStatistics &MFStats) const {
    auto Reqs = getSpecificFinalGenReqs(MFStats);
    if (FinalInstrDesc)
      Reqs.emplace_back(RequestLimit::NumInstrs{1},
                        FinalInstPolicy(FinalInstrDesc->getOpcode()));
    return Reqs;
  }

  std::vector<InstructionGroupRequest>
  getSpecificFinalGenReqs(const GenerationStatistics &MFStats) const {
    // No specific final requests if limit is already reached or we have
    // NumInstr limit.
    if (Limit.isNumLimit() || Limit.isReached(MFStats))
      return {};
    std::vector<InstructionGroupRequest> Reqs;
    auto &MBB = MF->back();
    auto &ProgCtx = GC->getProgramContext();
    auto &SnpTgt = ProgCtx.getLLVMState().getSnippyTarget();
    auto &&GP = DefaultGenPolicy(ProgCtx, GC->getGenSettings(),
                                 SnpTgt.getDefaultPolicyFilter(ProgCtx, MBB),
                                 SnpTgt.groupMustHavePrimaryInstr(ProgCtx, MBB),
                                 SnpTgt.getPolicyOverrides(ProgCtx, MBB), {});
    if (Limit.isSizeLimit()) {
      auto SizeLeft = Limit.getSizeLeft(MFStats);
      Reqs.emplace_back(RequestLimit::Size{SizeLeft}, std::move(GP));
    } else if (Limit.isMixedLimit()) {
      auto NumInstrsLeft = Limit.getNumInstrsLeft(MFStats);
      Reqs.emplace_back(RequestLimit::NumInstrs{NumInstrsLeft}, std::move(GP));
    }
    return Reqs;
  }

  void print(raw_ostream &OS, size_t Indent = 0) const {
    OS.indent(Indent) << "FunctionGenerationRequest<" << Limit.getAsString()
                      << ">(" << MF->getName() << ")\n";
    for_each(*this, [&](auto &Pair) {
      auto &[MBB, Req] = Pair;
      Req.print(OS, Indent + SubReqIndentSize);
    });
  }
};

} // namespace planning
} // namespace snippy
} // namespace llvm
