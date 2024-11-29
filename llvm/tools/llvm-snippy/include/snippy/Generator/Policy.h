//===-- Policy.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file Header containing Generation Policy "concept"
///
/// The primary class defined in this header is GenPolicy, which serves as a
/// holder for any class or struct that meets certain requirements. We will
/// refer to these qualifying classes as "Policies." To be considered a Policy,
/// a class must implement specific free functions. For an abstract class
/// named "MyPolicy," the signatures of these functions are as follows:
///
/// 1.
/// std::optional<InstructionRequest> next(MyPolicy &);
///
/// The "next" function serves as a mechanism by which code generation functions
/// request the next InstructionRequest to fulfill. It can return std::nullopt
/// if the MyPolicy class can only generate a limited number of requests. A
/// return of std::nullopt would indicate the end of the instruction group.
///
/// 2.
/// void initialize(MyPolicy &, InstructionGenerationContext &,
///                const RequestLimit &);
///
/// The "initialize" function acts as a hook into the generation process, called
/// before generating any instructions in the group. This function allows
/// MyPolicy to perform any necessary preparations before generating the group.
/// These preparations might include initializing registers or memory, setting
/// certain control and status registers (CSRs), etc. The function takes the
/// current generation context as an argument so that MyPolicy can prepare for
/// generation using that information (e.g., which instructions were already
/// generated in the basic block or which CSRs have specific values). MyPolicy
/// might only be able to generate a finite number of InstructionRequests. For
/// this reason, the "initialize" function takes a RequestLimit argument to
/// determine how many instructions (requested size or anything else) are
/// planned to be generated using that policy and prepare the required number of
/// InstructionRequests.
///
/// 3.
/// void finalize(MyPolicy &Policy, InstructionGenerationContext &);
///
/// The "finalize" function acts as the counterpart to "initialize" and is
/// called after generating all the instructions in the group. MyPolicy can
/// perform any required finalization actions in this function.
///
/// 4.
/// bool isInseparableBundle(MyPolicy &);
///
/// The "isInseparableBundle" function indicates whether generated
/// instructions must be executed one by one (on the simulator) instead of
/// executing the entire group when generation is complete. This functionality
/// is useful for SelfCheck and other tracking modes.
///
/// 5.
/// void print(MyPolicy &, raw_ostream &);
///
/// The "print" function is used solely for debugging purposes. It prints any
/// necessary information about MyPolicy.
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Config/ImmediateHistogram.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Config/RegisterHistogram.h"
#include "snippy/Generator/GenerationLimit.h"
#include "snippy/Generator/SelfCheckInfo.h"
#include "snippy/Generator/SnippyModule.h"
#include "snippy/Support/OpcodeGenerator.h"

#include <functional>
#include <memory>
#include <unordered_set>

template <> struct std::hash<llvm::MCRegister> {
  std::size_t operator()(const llvm::MCRegister &Reg) const {
    return std::hash<unsigned>{}(Reg);
  }
};

namespace llvm {
class MachineLoopInfo;

namespace snippy {

class GeneratorContext;
class RegPoolWrapper;
class CallGraphState;
class MemAccessInfo;
class SnippyLoopInfo;
struct SimulatorContext;
struct SnippyFunctionMetadata;

namespace planning {

// Helper class to keep additional information about the operand: register
// number that has been somehow selected before instruction generation,
// immediate operand range and so on.
class PreselectedOpInfo {
  using EmptyTy = std::monostate;
  using RegTy = llvm::Register;
  using ImmTy = StridedImmediate;
  using TiedTy = int;
  std::variant<EmptyTy, RegTy, ImmTy, TiedTy> Value;

  unsigned Flags = 0;

public:
  PreselectedOpInfo(llvm::Register R) : Value(R) {}
  PreselectedOpInfo(StridedImmediate Imm) : Value(Imm) {}
  PreselectedOpInfo() = default;
  bool isReg() const { return std::holds_alternative<RegTy>(Value); }
  bool isImm() const { return std::holds_alternative<ImmTy>(Value); }
  bool isUnset() const { return std::holds_alternative<EmptyTy>(Value); }
  bool isTiedTo() const { return std::holds_alternative<TiedTy>(Value); }

  unsigned getFlags() const { return Flags; }
  StridedImmediate getImm() const {
    assert(isImm());
    return std::get<ImmTy>(Value);
  }
  llvm::Register getReg() const {
    assert(isReg());
    return std::get<RegTy>(Value);
  }
  llvm::Register getTiedTo() const {
    assert(isTiedTo());
    return std::get<TiedTy>(Value);
  }

  void setFlags(unsigned F) { Flags = F; }
  void setTiedTo(int OpIdx) {
    assert(isUnset());
    Value = OpIdx;
  }

  friend constexpr bool operator==(const PreselectedOpInfo &Lhs,
                                   const PreselectedOpInfo &Rhs) {
    return Lhs.Value == Rhs.Value && Lhs.Flags == Rhs.Flags;
  }
};

class RegPoolStack {
private:
  struct PopDeleter {
    RegPoolStack *Parent;
    RegPoolWrapper *Prev;
    void operator()(RegPoolWrapper *Current) const {
      delete Current;
      Parent->Current = Prev;
    }
  };

public:
  RegPoolStack(SnippyProgramContext &ProgCtx) : ProgCtx(ProgCtx) {}

  auto append() {
    auto Ret = std::unique_ptr<RegPoolWrapper, PopDeleter>(
        new RegPoolWrapper(ProgCtx.getRegisterPool()),
        PopDeleter{this, Current});
    Current = Ret.get();
    return Ret;
  }

  auto &getCurrent() {
    assert(Current);
    return *Current;
  }

private:
  SnippyProgramContext &ProgCtx;
  RegPoolWrapper *Current = nullptr;
};

class InstructionGenerationContext final {
private:
  std::unique_ptr<SimulatorContext> NullSimCtx;

public:
  MachineBasicBlock &MBB;
  MachineBasicBlock::iterator Ins;
  GeneratorContext &GC;
  const SimulatorContext &SimCtx;
  GenerationStatistics Stats;
  SnippyFunctionMetadata *SFM = nullptr;
  MachineLoopInfo *MLI = nullptr;
  const CallGraphState *CGS = nullptr;
  MemAccessInfo *MAI = nullptr;
  const SnippyLoopInfo *SLI = nullptr;
  std::unordered_set<MCRegister> PotentialNaNs{};
  unsigned SizeErrorCount = 0;
  unsigned BacktrackCount = 0;

  InstructionGenerationContext(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator Ins,
                               GeneratorContext &GC,
                               const SimulatorContext &SimCtx);
  InstructionGenerationContext(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator Ins,
                               GeneratorContext &GC);
  ~InstructionGenerationContext();

  InstructionGenerationContext &append(SnippyFunctionMetadata *NewSFM) {
    SFM = NewSFM;
    return *this;
  }

  InstructionGenerationContext &append(MachineLoopInfo *NewMLI) {
    MLI = NewMLI;
    return *this;
  }

  InstructionGenerationContext &append(const CallGraphState *NewCGS) {
    CGS = NewCGS;
    return *this;
  }

  InstructionGenerationContext &append(MemAccessInfo *NewMAI) {
    MAI = NewMAI;
    return *this;
  }

  InstructionGenerationContext &append(const SnippyLoopInfo *NewSLI) {
    SLI = NewSLI;
    return *this;
  }

  auto pushRegPool() { return RPS.append(); }

  auto &getRegPool() { return RPS.getCurrent(); }

private:
  std::unique_ptr<RegPoolWrapper> TopRP;
  RegPoolStack RPS;
};

struct InstructionRequest final {
  unsigned Opcode;
  std::vector<PreselectedOpInfo> Preselected;
};

namespace detail {
struct EmptyFinalizeMixin {
  void finalize(InstructionGenerationContext &) {}
};
} // namespace detail

class DefaultGenPolicy final : public detail::EmptyFinalizeMixin {
  OpcGenHolder OpcGen;

public:
  DefaultGenPolicy(const DefaultGenPolicy &Other)
      : OpcGen(Other.OpcGen->copy()) {}

  DefaultGenPolicy(DefaultGenPolicy &&) = default;

  DefaultGenPolicy &operator=(const DefaultGenPolicy &Other) {
    DefaultGenPolicy Tmp = Other;
    std::swap(*this, Tmp);
    return *this;
  }

  DefaultGenPolicy &operator=(DefaultGenPolicy &&) = default;

  DefaultGenPolicy(const GeneratorContext &SGCtx,
                   std::function<bool(unsigned)> Filter,
                   bool MustHavePrimaryInstrs,
                   ArrayRef<OpcodeHistogramEntry> Overrides,
                   const std::unordered_map<unsigned, double> &WeightOverrides);

  std::optional<InstructionRequest> next() const {
    return InstructionRequest{OpcGen->generate(), {}};
  }
  void initialize(InstructionGenerationContext &InstrGenCtx,
                  const RequestLimit &Limit) const {}

  bool isInseparableBundle() const { return false; }

  void print(raw_ostream &OS) const { OS << "Default Generation Policy\n"; }
};

// This generation policy initializes the registers according to the
// valuegram-operands-regs option. That is, it inserts additional initializing
// instructions before each instruction for registers used in it that are not
// memory addresses. These initializing instructions are not taken into account
// in the planning instructions.
class ValuegramGenPolicy final : public detail::EmptyFinalizeMixin {
  OpcGenHolder OpcGen;

  std::vector<InstructionRequest> Instructions;
  unsigned Idx = 0;

public:
  ValuegramGenPolicy(const ValuegramGenPolicy &Other)
      : OpcGen(Other.OpcGen->copy()) {}

  ValuegramGenPolicy(ValuegramGenPolicy &&) = default;

  ValuegramGenPolicy &operator=(const ValuegramGenPolicy &Other) {
    ValuegramGenPolicy Tmp = Other;
    std::swap(*this, Tmp);
    return *this;
  }

  ValuegramGenPolicy &operator=(ValuegramGenPolicy &&) = default;

  ValuegramGenPolicy(
      const GeneratorContext &SGCtx, std::function<bool(unsigned)> Filter,
      bool MustHavePrimaryInstrs, ArrayRef<OpcodeHistogramEntry> Overrides,
      const std::unordered_map<unsigned, double> &WeightOverrides);

  std::optional<InstructionRequest> next() {
    assert(Idx <= Instructions.size());
    if (Idx < Instructions.size())
      return Instructions[Idx++];
    return std::nullopt;
  }

  void initialize(InstructionGenerationContext &InstrGenCtx,
                  const RequestLimit &Limit);

  bool isInseparableBundle() const { return true; }

  void print(raw_ostream &OS) const { OS << "Valuegram Generation Policy\n"; }

  std::vector<InstructionRequest>
  generateRegInit(InstructionGenerationContext &IGC, Register Reg,
                  const MCInstrDesc &InstrDesc);

  std::vector<InstructionRequest>
  generateOneInstrWithInitRegs(InstructionGenerationContext &IGC,
                               unsigned Opcode);

  APInt getValueFromValuegram(Register Reg, StringRef Prefix,
                              GeneratorContext &GC) const;
};

class BurstGenPolicy final : public detail::EmptyFinalizeMixin {
  std::vector<unsigned> Opcodes;
  std::discrete_distribution<size_t> Dist;
  std::vector<InstructionRequest> Instructions;
  unsigned Idx = 0;
  unsigned genOpc() { return Opcodes.at(Dist(RandEngine::engine())); }

public:
  std::optional<InstructionRequest> next() {
    if (Idx < Instructions.size())
      return Instructions[Idx++];
    return std::nullopt;
  }

  BurstGenPolicy(const GeneratorContext &SGCtx, unsigned BurstGroupID);

  void initialize(InstructionGenerationContext &InstrGenCtx,
                  const RequestLimit &Limit);

  void print(raw_ostream &OS) const {
    OS << "Burst Generation Policy: { ";
    for (auto [Opc, Prob] : zip(Opcodes, Dist.probabilities())) {
      OS << Opc << "(" << Prob << ") ";
    }
    OS << " }\n";
  }

  bool isInseparableBundle() const { return true; }
};

class FinalInstPolicy final : public detail::EmptyFinalizeMixin {
  unsigned Opcode;

public:
  FinalInstPolicy(unsigned Opc) : Opcode(Opc) {}

  bool isInseparableBundle() const { return false; }

  std::optional<InstructionRequest> next() const {
    return InstructionRequest{Opcode, {}};
  }

  void initialize(InstructionGenerationContext &InstrGenCtx,
                  const RequestLimit &Limit) const {}

  void print(raw_ostream &OS) const {
    OS << "Final Instruction Policy (" << Opcode << ")\n";
  }
};

template <typename PolicyTy>
std::optional<InstructionRequest> next(PolicyTy &Policy) {
  return Policy.next();
}

template <typename PolicyTy>
void initialize(PolicyTy &Policy, InstructionGenerationContext &InstrGenCtx,
                const RequestLimit &Limit) {
  Policy.initialize(InstrGenCtx, Limit);
}

template <typename PolicyTy>
void finalize(PolicyTy &Policy, InstructionGenerationContext &InstrGenCtx) {
  Policy.finalize(InstrGenCtx);
}

template <typename PolicyTy> bool isInseparableBundle(PolicyTy &Policy) {
  return Policy.isInseparableBundle();
}

template <typename PolicyTy> void print(PolicyTy &Pol, raw_ostream &OS) {
  Pol.print(OS);
}

///\class GenPolicy
///
/// A holder for any PolicyTy that meets the requirements described above.
/// Movable, copyable, has value semantics.
class GenPolicy final {
  struct Concept {
    virtual ~Concept() = default;
    virtual std::unique_ptr<Concept> copy() const = 0;
    virtual std::optional<InstructionRequest> next() = 0;
    virtual void initialize(InstructionGenerationContext &InstrGenCtx,
                            const RequestLimit &Limit) = 0;
    virtual void finalize(InstructionGenerationContext &InstrGenCtx) = 0;
    virtual bool isInseparableBundle() const = 0;
    virtual void print(raw_ostream &OS) const = 0;
  };

  template <typename PolicyTy> class Model final : public Concept {
    PolicyTy Data;
    static_assert(std::is_copy_constructible_v<std::decay_t<PolicyTy>>,
                  "Policy should be copy constructible.");

  public:
    Model(PolicyTy Pol) : Data(std::move(Pol)) {}

    std::unique_ptr<Concept> copy() const override {
      return std::make_unique<Model>(Data);
    }

    std::optional<InstructionRequest> next() override {
      return planning::next(Data);
    }

    void initialize(InstructionGenerationContext &InstrGenCtx,
                    const RequestLimit &Limit) override {
      planning::initialize(Data, InstrGenCtx, Limit);
    }

    void finalize(InstructionGenerationContext &InstrGenCtx) override {
      planning::finalize(Data, InstrGenCtx);
    }

    bool isInseparableBundle() const override {
      return planning::isInseparableBundle(Data);
    }

    void print(raw_ostream &OS) const override { planning::print(Data, OS); }
  };

  template <typename PolicyTy> Model(PolicyTy) -> Model<PolicyTy>;

  std::unique_ptr<Concept> Impl;

public:
  template <typename PolicyTy>
  GenPolicy(PolicyTy Pol)
      : Impl(std::make_unique<Model<PolicyTy>>(std::move(Pol))) {}

  GenPolicy(const GenPolicy &Other) : Impl(Other.Impl->copy()) {}

  GenPolicy(GenPolicy &&Other) = default;

  GenPolicy &operator=(const GenPolicy &Other) {
    GenPolicy Tmp = Other;
    std::swap(*this, Tmp);
    return *this;
  }

  GenPolicy &operator=(GenPolicy &&Other) = default;

  friend std::optional<InstructionRequest> next(const GenPolicy &Pol);

  friend void initialize(const GenPolicy &Pol,
                         InstructionGenerationContext &InstrGenCtx,
                         const RequestLimit &Limit);

  friend void finalize(const GenPolicy &Pol,
                       InstructionGenerationContext &InstrGenCtx);

  friend bool isInseparableBundle(const GenPolicy &Pol);

  friend void print(const GenPolicy &Pol, raw_ostream &OS);
};

inline std::optional<InstructionRequest> next(const GenPolicy &Pol) {
  return Pol.Impl->next();
}

inline void initialize(const GenPolicy &Pol,
                       InstructionGenerationContext &InstrGenCtx,
                       const RequestLimit &Limit) {
  return Pol.Impl->initialize(InstrGenCtx, Limit);
}

inline void finalize(const GenPolicy &Pol,
                     InstructionGenerationContext &InstrGenCtx) {
  return Pol.Impl->finalize(InstrGenCtx);
}
inline bool isInseparableBundle(const GenPolicy &Pol) {
  return Pol.Impl->isInseparableBundle();
}

inline void print(const GenPolicy &Pol, raw_ostream &OS) {
  Pol.Impl->print(OS);
}

} // namespace planning
} // namespace snippy
} // namespace llvm
