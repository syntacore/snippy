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

#include <optional>

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
  }

  bool isInseparableBundle() const {
    return planning::isInseparableBundle(Policy);
  }
};

class InstrGroupGenerationRAIIWrapper final {
  const InstructionGroupRequest &IG;
  InstructionGenerationContext &InstrGenCtx;

public:
  InstrGroupGenerationRAIIWrapper(const InstructionGroupRequest &Req,
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
  InstrRequestRange(const InstructionGroupRequest &Req,
                    GenerationStatistics &Stats)
      : Stats(Stats), Request(Req) {}

  auto begin() const { return detail::InstrRequestIterator(Request.policy()); }

  auto end() const {
    return detail::InstrRequestSentinel(Stats, Request.limit());
  }
};

constexpr auto SubReqIndentSize = 2;

/// \class SingleContextGroup
/// \brief The class wraps a vector of InstructionGroupRequest with a specific
/// mode changing policy.
/// Class is needed to define the execution context of instructions. They all
/// execute in a single context. This makes it easy to find the suitable context
/// for a specific instruction and insert it into basic block. It like a scope
/// block in a C++.
class SingleContextGroup final : private std::vector<InstructionGroupRequest> {
  // If there is no context or it does not make sense, then SingleContextGroup
  // simply represents a group of instructions.
  std::optional<ModeChangingInstPolicy> ModeChangingPolicy;

public:
  SingleContextGroup(ModeChangingInstPolicy &&MCPolicy)
      : ModeChangingPolicy(std::move(MCPolicy)) {}
  SingleContextGroup() = default;

  bool hasModeChange() const { return ModeChangingPolicy.has_value(); }
  std::optional<OpcodeFilterType> getOpcodeFilter() const {
    if (!ModeChangingPolicy.has_value())
      return std::nullopt;
    return ModeChangingPolicy->getOpcodeFilter();
  }
  InstructionGroupRequest createModeChangeIG() const {
    assert(hasModeChange());
    return planning::InstructionGroupRequest(
        planning::RequestLimit::NumInstrs{!ModeChangingPolicy->isSupport()},
        *ModeChangingPolicy);
  }
  size_t numIGs() const {
    // Because of one additional instruction group with mode change policy
    if (ModeChangingPolicy.has_value())
      return size() + 1;
    return size();
  }
  GenerationStatistics initialStats() const & {
    return std::accumulate(begin(), end(), GenerationStatistics{},
                           [](auto Acc, const auto &Entry) {
                             Acc.merge(Entry.initialStats());
                             return Acc;
                           });
  }
  RequestLimit limit() const & {
    auto MCNumInstr = static_cast<size_t>(hasModeChange() &&
                                          !ModeChangingPolicy->isSupport());
    using planning::RequestLimit;
    return std::accumulate(
        begin(), end(), RequestLimit{RequestLimit::NumInstrs{MCNumInstr}},
        [](auto Acc, const auto &Entry) { return Acc += Entry.limit(); });
  }

  using vector::begin;
  using vector::end;

  void print(raw_ostream &OS, size_t Indent = 0) const {
    bool HasModeChange = ModeChangingPolicy.has_value();
    OS.indent(Indent) << "SingleContextGroup <HasModeChange: " << HasModeChange;
    OS << ", Limit: " << limit().getAsString() << ">\n";
    for_each(*this, [&](const auto &Req) {
      Req.print(OS, Indent + SubReqIndentSize);
    });
  }

  void add(InstructionGroupRequest IG) { emplace_back(std::move(IG)); }
  void shuffle() { RandEngine::shuffle(begin(), end()); }
};

class BasicBlockRequest final : private std::vector<SingleContextGroup> {
  const MachineBasicBlock *MBB = nullptr;
  RequestLimit Limit;

public:
  BasicBlockRequest(const MachineBasicBlock *MBB)
      : MBB(MBB), Limit(RequestLimit::NumInstrs{}) {}

  const MachineBasicBlock *getMBB() const {
    assert(MBB);
    return MBB;
  }

  const RequestLimit &limit() const & { return Limit; }

  bool isLimitReached(const GenerationStatistics &Stats) const {
    return Limit.isReached(Stats);
  }

  size_t numIGs() const {
    return std::accumulate(begin(), end(), 0u, [](auto Acc, const auto &Entry) {
      return Acc + Entry.numIGs();
    });
  }

  using vector::begin;
  using vector::end;

  using vector::back;
  using vector::empty;
  using vector::size;

  using vector::iterator;
  using vector::operator[];

  void print(raw_ostream &OS, size_t Indent = 0) const {
    OS.indent(Indent) << "BasicBlockRequest<" << Limit.getAsString() << ">("
                      << MBB->getFullName() << ")\n";
    for_each(*this,
             [&](auto &Req) { Req.print(OS, Indent + SubReqIndentSize); });
  }

  // Add in last single context group
  void add(InstructionGroupRequest IG) {
    if (empty())
      emplace_back(SingleContextGroup{});
    Limit += IG.limit();
    back().add(std::move(IG));
  }

  void add(iterator It, InstructionGroupRequest IG) {
    Limit += IG.limit();
    It->add(std::move(IG));
  }

  void add(SingleContextGroup &&SG) {
    Limit += SG.limit();
    emplace_back(std::move(SG));
  }

  void shuffle() {
    for_each(*this, [](auto &SG) { SG.shuffle(); });
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

public:
  FunctionRequest(const MachineFunction &MFn, GeneratorContext &GC,
                  const MCInstrDesc *FinalInstrDesc = nullptr)
      // FIXME: Mixed limit there to accept both SizeLimit and NumInstrsLimit.
      : MF(&MFn), Limit(RequestLimit::Mixed{}), FinalInstrDesc(FinalInstrDesc),
        GC(&GC){};

  void setFinalInstr(const MCInstrDesc *Desc) { FinalInstrDesc = Desc; }

  SmallVector<size_t> getNumCtxGroupsPerMBBs(
      const std::vector<const MachineBasicBlock *> &Blocks) const {
    SmallVector<size_t> ModeChangesPerMBBs;
    ModeChangesPerMBBs.reserve(Blocks.size());
    transform(Blocks, std::back_inserter(ModeChangesPerMBBs),
              [&](const auto *MBB) {
                auto Found = map::find(MBB);
                auto &BBReq = Found->second;
                return BBReq.size();
              });
    return ModeChangesPerMBBs;
  }

  auto add(const MachineBasicBlock *MBB, BasicBlockRequest &&BB) {
    assert(MBB);
    checkLimitCompatibility(BB.limit());
    auto [It, WasInserted] = map::try_emplace(MBB, std::move(BB));
    assert(WasInserted);
    Limit += It->second.limit();
    return It;
  }

  // Here can be InstructionGroupRequest or SingleContextGroup
  template <typename GroupRequest>
  void addToBlock(const MachineBasicBlock *MBB, GroupRequest &&G) {
    assert(MBB);
    checkLimitCompatibility(G.limit());
    auto Found = map::find(MBB);
    if (Found != map::end()) {
      auto &BB = Found->second;
      Limit += G.limit();
      BB.add(std::move(G));
      return;
    }
    BasicBlockRequest BB(MBB);
    BB.add(std::move(G));
    add(MBB, std::move(BB));
  }

  void addToBlockIn(const MachineBasicBlock *MBB,
                    BasicBlockRequest::iterator It,
                    InstructionGroupRequest IG) {
    checkLimitCompatibility(IG.limit());
    auto Found = map::find(MBB);
    assert(Found != map::end());
    auto &BBReq = Found->second;
    assert(It >= BBReq.begin() && It < BBReq.end());
    Limit += IG.limit();
    BBReq.add(It, std::move(IG));
  }

  bool isLimitReached(const GenerationStatistics &Stats) const {
    return Limit.isReached(Stats);
  }

  const RequestLimit &limit() const & { return Limit; }

  BasicBlockRequest &get(const MachineBasicBlock *MBB) {
    assert(MBB);
    auto Found = map::find(MBB);
    assert(Found != map::end());
    return Found->second;
  }

  bool contains(const MachineBasicBlock *MBB) { return map::count(MBB); }

  using map::at;
  using map::begin;
  using map::end;

  std::vector<InstructionGroupRequest>
  getFinalGenReqs(const GenerationStatistics &MFStats) const {
    auto Reqs = getSpecificFinalGenReqs(MFStats);
    if (FinalInstrDesc)
      Reqs.emplace_back(RequestLimit::NumInstrs{1},
                        FinalInstPolicy(*GC->getConfig().CommonPolicyCfg,
                                        FinalInstrDesc->getOpcode()));
    return Reqs;
  }

  std::vector<InstructionGroupRequest>
  getSpecificFinalGenReqs(const GenerationStatistics &MFStats) const {
    // No specific final requests if limit is already reached or we have
    // NumInstr limit.
    if (Limit.isNumLimit() || Limit.isReached(MFStats))
      return {};
    std::vector<InstructionGroupRequest> Reqs;
    auto &ProgCtx = GC->getProgramContext();
    auto &&GP = DefaultGenPolicy(ProgCtx, GC->getConfig().DefFlowConfig);
    if (Limit.isSizeLimit()) {
      auto SizeLeft = Limit.getSizeLeft(MFStats);
      Reqs.emplace_back(RequestLimit::Size{SizeLeft}, std::move(GP));
    } else if (Limit.isMixedLimit()) {
      auto NumInstrsLeft = Limit.getNumInstrsLeft(MFStats);
      Reqs.emplace_back(RequestLimit::NumInstrs{NumInstrsLeft}, std::move(GP));
    }
    return Reqs;
  }

  void shuffle() {
    for_each(*this, [](auto &MBB) { MBB.second.shuffle(); });
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
