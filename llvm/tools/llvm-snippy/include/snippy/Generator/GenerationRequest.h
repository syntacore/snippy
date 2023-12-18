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
///   1. Only function generation requests should be created by hand.
///   2. You can get generation requests for a block from corresponding function
///      request.
///   3. Block generation request consists of instruction group requests. Only
///      instruction groups expected to be generated in block (except final).
///   4. When you generate instruction group you can get next generation opcode
///      corresponding to the current generation policy and check whether
///      request is completed. After successful opcode generation you should
///      check if you need to change policy and update it in the request.
///   5. After processing all blocks request for a function, final requests must
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

#include "snippy/Generator/BlockGenPlan.h"
#include "snippy/Generator/Policy.h"
#include "snippy/Target/Target.h"

namespace llvm {
namespace snippy {

struct GenerationResult;

raw_ostream &operator<<(raw_ostream &OS, GenerationMode GM);

// Return how many primary instrs were generated and size of all genrated instrs
struct GenerationStatistics final {
  size_t NumOfInstrs;
  size_t GeneratedSize;

  GenerationStatistics(size_t NumOfInstrs = 0, size_t GeneratedSize = 0)
      : NumOfInstrs(NumOfInstrs), GeneratedSize(GeneratedSize) {}

  void merge(const GenerationResult &Res);

  void merge(const GenerationStatistics &Statistics) {
    NumOfInstrs += Statistics.NumOfInstrs;
    GeneratedSize += Statistics.GeneratedSize;
  }

  void print(raw_ostream &OS) const;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

namespace detail {

struct ICompletableGenReq {
  [[nodiscard]] virtual bool
  isCompleted(const GenerationStatistics &CurrentStats) const = 0;
  [[nodiscard]] virtual std::optional<size_t>
  getGenerationLimit(GenerationMode GM) const = 0;
  virtual ~ICompletableGenReq() = default;
};

} // namespace detail

struct IInstrGroupGenReq : virtual public detail::ICompletableGenReq {
  [[nodiscard]] virtual bool
  canGenerateMore(const GenerationStatistics &CommittedStats,
                  size_t NewGeneratedCodeSize) const {
    return true;
  }

  virtual void changePolicy(GenPolicy NewPolicy) = 0;

  [[nodiscard]] virtual unsigned genOpc() = 0;

  // Burst group id from BurstGram groupings. If nullopt, then plain instruction
  // generation is required.
  [[nodiscard]] virtual std::optional<size_t> getBurstGroupID() const {
    return std::nullopt;
  }

  virtual void print(raw_ostream &OS, size_t Indent = 0) const = 0;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

struct IMBBGenReq;

class MBBGenReqIterator final {
  IMBBGenReq &Req;
  IInstrGroupGenReq *Elem;

public:
  using difference_type = void;
  using value_type = IInstrGroupGenReq;
  using pointer_type = IInstrGroupGenReq *;
  using reference_type = IInstrGroupGenReq &;
  using iterator_category = std::input_iterator_tag;

  MBBGenReqIterator(IMBBGenReq &Req);
  MBBGenReqIterator(IMBBGenReq &Req, std::nullptr_t);

  MBBGenReqIterator &operator++();

  IInstrGroupGenReq &operator*() {
    assert(Elem);
    return *Elem;
  }
  IInstrGroupGenReq *operator->() {
    assert(Elem);
    return Elem;
  }

  bool operator!=(const MBBGenReqIterator &Other) const {
    return !(*this == Other);
  }

  bool operator==(const MBBGenReqIterator &Other) const {
    return &Req == &Other.Req && Elem == Other.Elem;
  }
};

struct IMBBGenReq : virtual public detail::ICompletableGenReq {
  [[nodiscard]] virtual IInstrGroupGenReq *nextSubRequest() = 0;

  [[nodiscard]] virtual MBBGenReqIterator begin() = 0;
  [[nodiscard]] virtual MBBGenReqIterator end() = 0;

  virtual void print(raw_ostream &OS, size_t Indent = 0) const = 0;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

struct IFunctionGenReq : virtual public detail::ICompletableGenReq {
  [[nodiscard]] virtual IMBBGenReq &
  getMBBGenerationRequest(const MachineBasicBlock &MBB) = 0;

  [[nodiscard]] virtual std::vector<std::unique_ptr<IInstrGroupGenReq>>
  getFinalGenReqs(const GenerationStatistics &MFStats) const = 0;

  virtual void print(raw_ostream &OS, size_t Indent = 0) const = 0;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

namespace detail {

class NumInstrsGenReq : virtual public detail::ICompletableGenReq {
protected:
  // How many primary instructions we need to generate as a result of this
  // request
  size_t PrimaryInstrNum;

  static constexpr GenerationMode kGM = GenerationMode::NumInstrs;

public:
  NumInstrsGenReq(size_t PrimaryInstrNum) : PrimaryInstrNum(PrimaryInstrNum) {}

  bool isCompleted(const GenerationStatistics &CurrentStats) const override {
    return CurrentStats.NumOfInstrs >= PrimaryInstrNum;
  }

  std::optional<size_t> getGenerationLimit(GenerationMode GM) const override {
    return GM == kGM ? std::make_optional(PrimaryInstrNum) : std::nullopt;
  }
};

class SizeGenReq : virtual public detail::ICompletableGenReq {
protected:
  // Max size in bytes for this request
  size_t SizeLimit;

  static constexpr GenerationMode kGM = GenerationMode::Size;

public:
  SizeGenReq(size_t SizeLimitIn) : SizeLimit(SizeLimitIn) {}

  bool isCompleted(const GenerationStatistics &CurrentStats) const override {
    return CurrentStats.GeneratedSize >= SizeLimit;
  }

  std::optional<size_t> getGenerationLimit(GenerationMode GM) const override {
    return GM == kGM ? std::make_optional(SizeLimit) : std::nullopt;
  }
};

class MixedGenReq : public NumInstrsGenReq {
protected:
  static constexpr auto kGM = GenerationMode::Mixed;

public:
  MixedGenReq(size_t PrimaryInstrNum) : NumInstrsGenReq(PrimaryInstrNum) {}
};

template <GenerationMode> struct GenerationModeToRequestPolicy;

template <> struct GenerationModeToRequestPolicy<GenerationMode::NumInstrs> {
  using RequestPolicy = NumInstrsGenReq;
};

template <> struct GenerationModeToRequestPolicy<GenerationMode::Size> {
  using RequestPolicy = SizeGenReq;
};

template <> struct GenerationModeToRequestPolicy<GenerationMode::Mixed> {
  using RequestPolicy = MixedGenReq;
};

constexpr auto SubReqIndentSize = 2;

template <GenerationMode> class InstrGroupGenReq;

template <GenerationMode GM>
class InstrGroupGenReqImpl
    : public IInstrGroupGenReq,
      public GenerationModeToRequestPolicy<GM>::RequestPolicy {
protected:
  GenPolicy Policy;

  using RequestPolicy =
      typename GenerationModeToRequestPolicy<GM>::RequestPolicy;
  static_assert(RequestPolicy::kGM == GM);

public:
  InstrGroupGenReqImpl(size_t Limit, GenPolicy &&PolicyIn)
      : IInstrGroupGenReq(), RequestPolicy(Limit), Policy(std::move(PolicyIn)) {
    assert(Policy && "Empty policy!");
  }

  InstrGroupGenReqImpl(const InstrGroupGenReqImpl &) = delete;
  InstrGroupGenReqImpl(InstrGroupGenReqImpl &&) = delete;

  void changePolicy(GenPolicy NewPolicy) override {
    assert(NewPolicy && "Empty new policy!");
    Policy.swap(NewPolicy);
    assert(Policy);
  }

  unsigned genOpc() override { return Policy->genNextOpc(); }

  void print(raw_ostream &OS, size_t Indent) const override {
    OS.indent(Indent) << "InstrGroupGenerationRequest<" << GM
                      << ">: " << getGenerationLimit(GM) << "\n";
  }
};

template <>
class InstrGroupGenReq<GenerationMode::NumInstrs> final
    : public InstrGroupGenReqImpl<GenerationMode::NumInstrs> {
public:
  InstrGroupGenReq(size_t NumInstr, GenPolicy &&PolicyIn)
      : InstrGroupGenReqImpl(NumInstr, std::move(PolicyIn)) {}
};

template <>
class InstrGroupGenReq<GenerationMode::Size> final
    : public InstrGroupGenReqImpl<GenerationMode::Size> {
public:
  InstrGroupGenReq(size_t SizeLimit, GenPolicy &&PolicyIn)
      : InstrGroupGenReqImpl(SizeLimit, std::move(PolicyIn)) {}

  bool canGenerateMore(const GenerationStatistics &CommittedStats,
                       size_t NewGeneratedCodeSize) const override {
    return CommittedStats.GeneratedSize + NewGeneratedCodeSize <= SizeLimit;
  }
};

class BurstGroupGenReq final
    : public InstrGroupGenReqImpl<GenerationMode::NumInstrs> {
  size_t BurstGroupId;

public:
  BurstGroupGenReq(size_t NumInstr, size_t GroupId, GenPolicy &&PolicyIn)
      : InstrGroupGenReqImpl(NumInstr, std::move(PolicyIn)),
        BurstGroupId(GroupId) {}

  std::optional<size_t> getBurstGroupID() const override {
    return BurstGroupId;
  }

  void print(raw_ostream &OS, size_t Indent) const override {
    OS.indent(Indent) << "BurstGroupGenerationRequest: " << PrimaryInstrNum
                      << "\n";
  }
};

template <GenerationMode GM>
class MBBGenReq final
    : public IMBBGenReq,
      public GenerationModeToRequestPolicy<GM>::RequestPolicy {
protected:
  using RequestPolicy =
      typename GenerationModeToRequestPolicy<GM>::RequestPolicy;
  static_assert(RequestPolicy::kGM == GM);

  using SubReqStorage = std::vector<std::unique_ptr<IInstrGroupGenReq>>;
  GeneratorContext &SGCtx;
  const MachineBasicBlock &MBB;
  SubReqStorage SubReqs;
  size_t SubReqsGenerated = 0;

  auto getPolicy(std::optional<unsigned> BurstGroupID = std::nullopt) const {
    auto &SnpTgt = SGCtx.getLLVMState().getSnippyTarget();
    return SnpTgt.getGenerationPolicy(MBB, SGCtx, BurstGroupID);
  }

  auto getFinalInstPolicy(unsigned Opc) const {
    return std::make_unique<FinalInstPolicy>(Opc);
  }

public:
  MBBGenReq(const SingleBlockGenPlanTy &Plan, const MachineBasicBlock &MBB,
            GeneratorContext &SGCtx)
      : IMBBGenReq(), RequestPolicy(Plan.limit(GM).value()), SGCtx(SGCtx),
        MBB(MBB) {
    assert(GM == Plan.genMode());
    auto &Packs = Plan.packs();
    auto CreateSubReq =
        [this](const InstPackTy &Pack) -> SubReqStorage::value_type {
      auto Limit = Pack.getLimit();
      if (Pack.isBurst()) {
        auto GroupId = Pack.getGroupId();
        return std::make_unique<BurstGroupGenReq>(Limit, GroupId,
                                                  getPolicy(GroupId));
      }
      return std::make_unique<InstrGroupGenReq<GM>>(Limit, getPolicy());
    };

    std::transform(Packs.begin(), Packs.end(), std::back_inserter(SubReqs),
                   CreateSubReq);
  }

  MBBGenReqIterator begin() override { return {*this}; }
  MBBGenReqIterator end() override { return {*this, nullptr}; }

  IInstrGroupGenReq *nextSubRequest() override {
    if (SubReqsGenerated >= SubReqs.size())
      return nullptr;

    auto &SubReq = SubReqs[SubReqsGenerated];
    assert(SubReq);
    ++SubReqsGenerated;
    return SubReq.get();
  }

  void print(raw_ostream &OS, size_t Indent) const override {
    OS.indent(Indent) << "MBBGenerationRequest<" << GM << ">("
                      << MBB.getFullName() << "): " << getGenerationLimit(GM)
                      << "\n";
    for (auto &&Req : SubReqs)
      Req->print(OS, Indent + SubReqIndentSize);
  }
};

template <GenerationMode GM>
class FunctionGenReqImpl
    : public IFunctionGenReq,
      public GenerationModeToRequestPolicy<GM>::RequestPolicy {
protected:
  using RequestPolicy =
      typename GenerationModeToRequestPolicy<GM>::RequestPolicy;

  // We store all generation requests as unique pointers because mixed
  // generation request can contain both num instrs requests and size requests
  using MBBGenReqT = std::unique_ptr<IMBBGenReq>;
  // We need ordered map with special comparator to make print reproducible
  using ReqsMap = std::map<const MachineBasicBlock *, MBBGenReqT, MIRComp>;

  static_assert(RequestPolicy::kGM == GM);

  const MCInstrDesc *FinalInstDesc;
  const MachineFunction &MF;
  ReqsMap BBReqs;
  GeneratorContext &SGCtx;

  virtual std::vector<std::unique_ptr<IInstrGroupGenReq>>
  getSpecificFinalGenReqs(const GenerationStatistics &MFStats) const {
    return {};
  }

public:
  FunctionGenReqImpl(const MachineFunction &MF, const BlocksGenPlanTy &Plan,
                     const MCInstrDesc *FinalInstDesc, GeneratorContext &SGCtx)
      : IFunctionGenReq(), RequestPolicy([&Plan] {
          return std::accumulate(
              Plan.begin(), Plan.end(), size_t(0), [](size_t Acc, auto &Elem) {
                auto GMToCount = GM == GenerationMode::Mixed
                                     ? GenerationMode::NumInstrs
                                     : GM;
                return Acc + Elem.second.limit(GMToCount).value_or(0);
              });
        }()),
        FinalInstDesc(FinalInstDesc), MF(MF), BBReqs(), SGCtx(SGCtx) {
    std::transform(
        MF.begin(), MF.end(), std::inserter(BBReqs, BBReqs.end()),
        [&](const auto &MBB)
            -> std::pair<const MachineBasicBlock *, MBBGenReqT> {
          const SingleBlockGenPlanTy &BBPlan = Plan.at(&MBB);
          if (BBPlan.genMode() == GenerationMode::NumInstrs) {
            assert(GM != GenerationMode::Size &&
                   "Num instrs generation mode for block is incompatible with "
                   "function generation by size");
            return std::make_pair(
                &MBB,
                std::make_unique<detail::MBBGenReq<GenerationMode::NumInstrs>>(
                    BBPlan, MBB, SGCtx));
          }
          assert(BBPlan.genMode() == GenerationMode::Size &&
                 "BBPlan generation mode can be either num instrs or size");
          assert(GM != GenerationMode::NumInstrs &&
                 "Size generation mode for block is incompatible with function "
                 "generation by num instrs");
          return std::make_pair(
              &MBB, std::make_unique<detail::MBBGenReq<GenerationMode::Size>>(
                        BBPlan, MBB, SGCtx));
        });
  }

  IMBBGenReq &getMBBGenerationRequest(const MachineBasicBlock &MBB) override {
    return *BBReqs.at(&MBB);
  }

  std::vector<std::unique_ptr<IInstrGroupGenReq>>
  getFinalGenReqs(const GenerationStatistics &MFStats) const override {
    auto Reqs = getSpecificFinalGenReqs(MFStats);
    if (FinalInstDesc)
      Reqs.emplace_back(
          std::make_unique<detail::InstrGroupGenReq<GenerationMode::NumInstrs>>(
              1,
              std::make_unique<FinalInstPolicy>(FinalInstDesc->getOpcode())));
    return Reqs;
  }

  void print(raw_ostream &OS, size_t Indent) const override {
    OS.indent(Indent) << "FunctionGenerationRequest<" << GM << ">("
                      << MF.getName() << "): " << getGenerationLimit(GM)
                      << "\n";
    for (auto &[MBB, Req] : BBReqs)
      Req->print(OS, Indent + SubReqIndentSize);
  }
};

} // namespace detail

template <GenerationMode> class FunctionGenReq;

template <>
class FunctionGenReq<GenerationMode::NumInstrs> final
    : public detail::FunctionGenReqImpl<GenerationMode::NumInstrs> {
  using BaseT = FunctionGenReqImpl<GenerationMode::NumInstrs>;

public:
  FunctionGenReq(const MachineFunction &MF, const BlocksGenPlanTy &Plan,
                 const MCInstrDesc *FinalInstDesc, GeneratorContext &SGCtx)
      : FunctionGenReqImpl(MF, Plan, FinalInstDesc, SGCtx) {}
};

template <>
class FunctionGenReq<GenerationMode::Size> final
    : public detail::FunctionGenReqImpl<GenerationMode::Size> {
  using BaseT = FunctionGenReqImpl<GenerationMode::Size>;

public:
  FunctionGenReq(const MachineFunction &MF, const BlocksGenPlanTy &Plan,
                 const MCInstrDesc *FinalInstDesc, GeneratorContext &SGCtx)
      : FunctionGenReqImpl(MF, Plan, FinalInstDesc, SGCtx) {}

  std::vector<std::unique_ptr<IInstrGroupGenReq>>
  getSpecificFinalGenReqs(const GenerationStatistics &MFStats) const override {
    assert(MFStats.GeneratedSize <= SizeLimit);
    std::vector<std::unique_ptr<IInstrGroupGenReq>> Reqs;
    auto SizeLeft = SizeLimit - MFStats.GeneratedSize;
    auto &SnpTgt = SGCtx.getLLVMState().getSnippyTarget();
    auto &&GP = SnpTgt.getGenerationPolicy(MF.back(), SGCtx, std::nullopt);
    Reqs.emplace_back(
        std::make_unique<detail::InstrGroupGenReq<GenerationMode::Size>>(
            SizeLeft, std::move(GP)));
    return Reqs;
  }
};

template <>
class FunctionGenReq<GenerationMode::Mixed> final
    : public detail::FunctionGenReqImpl<GenerationMode::Mixed> {
  using BaseT = FunctionGenReqImpl<GenerationMode::Size>;

public:
  FunctionGenReq(const MachineFunction &MF, const BlocksGenPlanTy &Plan,
                 const MCInstrDesc *FinalInstDesc, GeneratorContext &SGCtx)
      : FunctionGenReqImpl(MF, Plan, FinalInstDesc, SGCtx) {}

  std::vector<std::unique_ptr<IInstrGroupGenReq>>
  getSpecificFinalGenReqs(const GenerationStatistics &MFStats) const override {
    if (MFStats.NumOfInstrs >= PrimaryInstrNum)
      return {};

    std::vector<std::unique_ptr<IInstrGroupGenReq>> Reqs;
    auto NumInstrsLeft = PrimaryInstrNum - MFStats.NumOfInstrs;
    auto &SnpTgt = SGCtx.getLLVMState().getSnippyTarget();
    auto GP = SnpTgt.getGenerationPolicy(MF.back(), SGCtx,
                                         /* BurstGroupID */ std::nullopt);
    Reqs.emplace_back(
        std::make_unique<detail::InstrGroupGenReq<GenerationMode::NumInstrs>>(
            NumInstrsLeft, std::move(GP)));
    return Reqs;
  }
};

} // namespace snippy
} // namespace llvm
