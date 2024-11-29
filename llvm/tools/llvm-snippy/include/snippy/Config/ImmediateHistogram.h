//===-- ImmediateHistogram.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This file implements functionality for sampling immediate values from a
/// histogram.
///
//===----------------------------------------------------------------------===//
//
#pragma once
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/OpcodeCache.h"
#include "snippy/Support/RandUtil.h"
#include "snippy/Support/YAMLUtils.h"

#include "snippy/Config/OpcodeHistogram.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"

#include <unordered_map>
#include <variant>
#include <vector>

namespace llvm {
namespace snippy {

struct ImmediateHistogramEntry final {
  int Value;
  double Weight;
};

struct ImmediateHistogramSequence final {
  std::vector<int> Values;
  std::vector<double> Weights;
};

class ImmHistOpcodeSettings final {
public:
  enum class Kind { Uniform, Custom };

private:
  std::optional<ImmediateHistogramSequence> Seq;
  Kind SettingsKind = Kind::Uniform;

public:
  ImmHistOpcodeSettings() = default;

  ImmHistOpcodeSettings(const ImmediateHistogramSequence &Sequence)
      : Seq(Sequence), SettingsKind(Kind::Custom) {}

  ImmHistOpcodeSettings(ImmediateHistogramSequence &&Sequence)
      : Seq(std::move(Sequence)), SettingsKind(Kind::Custom) {}

  bool isUniform() const { return SettingsKind == Kind::Uniform; }

  bool isSequence() const { return SettingsKind == Kind::Custom; }

  const ImmediateHistogramSequence &getSequence() const {
    assert(isSequence() && Seq.has_value());
    return *Seq;
  }

  Kind getKind() const { return SettingsKind; }
};

struct ImmHistConfigForRegEx final {
  std::string Expr;
  ImmHistOpcodeSettings Data;
};

struct ImmediateHistogramRegEx final {
  std::vector<ImmHistConfigForRegEx> Exprs;
};

class ImmediateHistogram final {
  // std::variant for backwards compatibility
  std::variant<ImmediateHistogramSequence, ImmediateHistogramRegEx> Impl;

public:
  ImmediateHistogram() = default;

  ImmediateHistogram(const ImmediateHistogramSequence &Seq) : Impl(Seq) {}

  ImmediateHistogram(const ImmediateHistogramRegEx &RegEx) : Impl(RegEx) {}

  template <typename AltTy> bool holdsAlternative() const {
    return std::holds_alternative<AltTy>(Impl);
  }

  template <typename AltTy> const AltTy &get() const {
    return std::get<AltTy>(Impl);
  }
};

class StridedImmediate {
public:
  using ValueType = int64_t;
  using StrideType = uint64_t;

private:
  ValueType Min = 0;
  ValueType Max = 0;
  StrideType Stride = 0;
  bool Init = false;

public:
  StridedImmediate(ValueType MinIn, ValueType MaxIn, StrideType StrideIn)
      : Min(MinIn), Max(MaxIn), Stride(StrideIn), Init(true) {
    assert(Min <= Max);
    assert(Stride == 0 || isPowerOf2_64(Stride));
  }
  StridedImmediate() = default;

  bool isInitialized() const { return Init; }
  auto getMin() const {
    assert(isInitialized());
    return Min;
  }
  auto getMax() const {
    assert(isInitialized());
    return Max;
  }
  auto getStride() const {
    assert(isInitialized());
    return Stride;
  }

  bool operator==(const StridedImmediate &Imm) const {
    if (isInitialized() != Imm.isInitialized())
      return false;
    if (!isInitialized())
      return true;
    return getMin() == Imm.getMin() && getMax() == Imm.getMax() &&
           getStride() == Imm.getStride();
  }
};

template <int MinValue, int MaxValue>
int genImmInInterval(const ImmediateHistogramSequence &IH) {
  static_assert(MinValue <= MaxValue);
  assert(std::is_sorted(IH.Values.begin(), IH.Values.end()));
  assert(IH.Values.size() == IH.Weights.size());
  auto First = std::lower_bound(IH.Values.begin(), IH.Values.end(), MinValue);
  auto Last = std::upper_bound(IH.Values.begin(), IH.Values.end(), MaxValue);
  if (First == Last)
    snippy::fatal("Immediate histogram",
                  formatv("it does not contain any values in [{0}; {1}]",
                          MinValue, MaxValue));

  auto FirstIdx = std::distance(IH.Values.begin(), First);
  auto LastIdx = std::distance(IH.Values.begin(), Last);
  std::discrete_distribution<unsigned> Dist(IH.Weights.begin() + FirstIdx,
                                            IH.Weights.begin() + LastIdx);
  auto Offset = Dist(RandEngine::engine());
  return IH.Values[FirstIdx + Offset];
}

enum class ImmediateSignedness {
  Signed,
  Unsigned,
};

enum class ImmediateZero {
  Include,
  Exclude,
};

template <int MinValue, int MaxValue>
int genImmInInterval(const StridedImmediate &StridedImm) {
  assert(StridedImm.isInitialized());
  using ValueType = StridedImmediate::ValueType;
  auto Min = std::clamp<ValueType>(StridedImm.getMin(), MinValue, MaxValue);
  auto Max = std::clamp<ValueType>(StridedImm.getMax(), MinValue, MaxValue);
  auto Stride = StridedImm.getStride();
  assert(Stride != 0);
  assert(Max >= Min);

  // Min may be either MinValue or StridedImm.getMin() depending on which value
  // is bigger (see a few lines above). However alignments of these values are
  // not guaranteed to match. Below we make sure that Min is properly aligned,
  // i.e. Min % Stride == StridedImm.getMin() % Stride. The same must be done
  // for Max.
  auto SkewMin = std::abs(StridedImm.getMin());
  auto SkewMax = std::abs(StridedImm.getMax());
  Min = (Min < 0 ? -1ll : 1ll) * alignTo(std::abs(Min), Stride, SkewMin);
  Max = (Max < 0 ? -1ll : 1ll) * alignDown(std::abs(Max), Stride, SkewMax);
  assert(Max >= Min);
  assert((StridedImm.getMin() - Min) % Stride == 0);
  assert((StridedImm.getMax() - Max) % Stride == 0);

  auto NMax = (Max - Min) / Stride;
  auto N = RandEngine::genInInterval(NMax);
  auto Imm = Min + N * Stride;

  return Imm;
}

template <unsigned BitWidth, ImmediateSignedness Sign, ImmediateZero Zero,
          unsigned ZeroBits>
auto genImmInInterval(const ImmediateHistogramSequence &IH) {
  constexpr unsigned NumBits = BitWidth - ZeroBits;
  constexpr bool IsSigned = Sign == ImmediateSignedness::Signed;
  constexpr bool IncludeZero = Zero == ImmediateZero::Include;
  static_assert(ZeroBits > 0 || (IsSigned && !IncludeZero));
  static_assert(NumBits < 10,
                "Current implementation of genImmInInterval reserves space on "
                "the stack proportional to 1 << NumBits");
  assert(std::is_sorted(IH.Values.begin(), IH.Values.end()));
  assert(IH.Values.size() == IH.Weights.size());
  SmallVector<int, 1 << NumBits> Values;
  SmallVector<double, 1 << NumBits> Weights;

  size_t Idx = 0;
  auto PushInInterval = [&](int Min, int Max) {
    int Step = 1 << ZeroBits;
    for (int Val = Min; Val <= Max; Val += Step) {
      while (Idx < IH.Values.size() && IH.Values[Idx] < Val)
        Idx++;
      if (Idx == IH.Values.size())
        return;
      if (IH.Values[Idx] == Val) {
        Values.push_back(IH.Values[Idx]);
        Weights.push_back(IH.Weights[Idx]);
      }
    }
  };

  if (IsSigned) {
    int Min = -(1 << (BitWidth - 1));
    int Max = -(1 << ZeroBits);
    PushInInterval(Min, Max);
  }
  if (IncludeZero)
    PushInInterval(0, 0);
  {
    int Min = 1 << ZeroBits;
    int Max = ((1 << (NumBits - 1)) - 1) << ZeroBits;
    PushInInterval(Min, Max);
  }

  if (Values.empty())
    snippy::fatal("Immediate histogram",
                  "it does not contain any suitable values.");

  std::discrete_distribution<unsigned> Dist(Weights.begin(), Weights.end());

  return Values[Dist(RandEngine::engine())];
}

template <unsigned BitWidth, ImmediateSignedness Sign, ImmediateZero Zero,
          unsigned ZeroBits>
auto genImmInInterval() {
  constexpr unsigned NumBits = BitWidth - ZeroBits;
  constexpr bool IsSigned = Sign == ImmediateSignedness::Signed;
  constexpr bool IncludeZero = Zero == ImmediateZero::Include;
  auto Num = [] {
    if (IsSigned)
      return RandEngine::genInInterval(-(1 << (NumBits - 1)),
                                       (1 << (NumBits - 1)) - 1 - !IncludeZero);
    return RandEngine::genInInterval(0, (1 << NumBits) - 1 - !IncludeZero);
  }();
  if (!IncludeZero && Num >= 0)
    Num++;
  return Num << ZeroBits;
}

template <int MinValue, int MaxValue>
unsigned genImmInInterval(const ImmediateHistogramSequence *IH,
                          const StridedImmediate &StridedImm) {
  if (StridedImm.isInitialized())
    return genImmInInterval<MinValue, MaxValue>(StridedImm);
  if (IH)
    return genImmInInterval<MinValue, MaxValue>(*IH);
  return RandEngine::genInInterval(MinValue, MaxValue);
}

template <unsigned BitWidth, ImmediateSignedness Sign, ImmediateZero Zero,
          unsigned ZeroBits = 0>
auto genImmInInterval(const ImmediateHistogramSequence *IH,
                      const StridedImmediate &StridedImm) {
  if (StridedImm.isInitialized()) {
    unsigned MinStride = 1 << ZeroBits;
    assert(isPowerOf2_64(StridedImm.getStride()));
    assert(StridedImm.getStride() >= MinStride);
    assert(StridedImm.getMin() % MinStride == 0);
    assert(StridedImm.getMax() % MinStride == 0);
    assert(Zero == ImmediateZero::Include);
    if constexpr (Sign == ImmediateSignedness::Signed) {
      return genImmInInterval<-(1 << (BitWidth - 1)),
                              (1 << (BitWidth - 1)) - 1>(StridedImm);
    } else {
      return genImmInInterval<0, (1 << BitWidth) - 1>(StridedImm);
    }
  }
  if (IH)
    return genImmInInterval<BitWidth, Sign, Zero, ZeroBits>(*IH);
  return genImmInInterval<BitWidth, Sign, Zero, ZeroBits>();
}

template <unsigned BitWidth>
unsigned genImmUINT(const ImmediateHistogramSequence *IH,
                    const StridedImmediate &StridedImm) {
  return genImmInInterval<0, (1 << BitWidth) - 1>(IH, StridedImm);
}

template <unsigned BitWidth, unsigned ZeroBits>
unsigned genImmUINTWithNZeroLSBs(const ImmediateHistogramSequence *IH,
                                 const StridedImmediate &StridedImm) {
  return genImmInInterval<BitWidth, ImmediateSignedness::Unsigned,
                          ImmediateZero::Include, ZeroBits>(IH, StridedImm);
}

template <unsigned BitWidth>
unsigned genImmNonZeroUINT(const ImmediateHistogramSequence *IH,
                           const StridedImmediate &StridedImm) {
  return genImmInInterval<1, (1 << BitWidth) - 1>(IH, StridedImm);
}

template <unsigned BitWidth, unsigned ZeroBits>
unsigned genImmNonZeroUINTWithNZeroLSBs(const ImmediateHistogramSequence *IH,
                                        const StridedImmediate &StridedImm) {
  return genImmInInterval<BitWidth, ImmediateSignedness::Unsigned,
                          ImmediateZero::Exclude, ZeroBits>(IH, StridedImm);
}

template <unsigned BitWidth>
int genImmSINT(const ImmediateHistogramSequence *IH,
               const StridedImmediate &StridedImm) {
  return genImmInInterval<-(1 << (BitWidth - 1)), (1 << (BitWidth - 1)) - 1>(
      IH, StridedImm);
}

template <unsigned BitWidth, unsigned ZeroBits>
int genImmSINTWithNZeroLSBs(const ImmediateHistogramSequence *IH,
                            const StridedImmediate &StridedImm) {
  return genImmInInterval<BitWidth, ImmediateSignedness::Signed,
                          ImmediateZero::Include, ZeroBits>(IH, StridedImm);
}

template <unsigned BitWidth>
int genImmNonZeroSINT(const ImmediateHistogramSequence *IH,
                      const StridedImmediate &StridedImm) {
  return genImmInInterval<BitWidth, ImmediateSignedness::Signed,
                          ImmediateZero::Exclude>(IH, StridedImm);
}

template <unsigned BitWidth, unsigned ZeroBits>
int genImmNonZeroSINTWithNZeroLSBs(const ImmediateHistogramSequence *IH,
                                   const StridedImmediate &StridedImm) {
  return genImmInInterval<BitWidth, ImmediateSignedness::Signed,
                          ImmediateZero::Exclude, ZeroBits>(IH, StridedImm);
}

template <unsigned BitWidth, int Offset>
int genImmSINTWithOffset(const ImmediateHistogramSequence *IH,
                         const StridedImmediate &StridedImm) {
  return genImmInInterval<Offset - (1 << (BitWidth - 1)),
                          (1 << (BitWidth - 1)) - 1 + Offset>(IH, StridedImm);
}

class OpcodeToImmHistSequenceMap final {
  std::unordered_map<unsigned, ImmHistOpcodeSettings> Data;

public:
  OpcodeToImmHistSequenceMap(const ImmediateHistogramRegEx &ImmHist,
                             const OpcodeHistogram &OpcHist,
                             const OpcodeCache &OpCC);

  OpcodeToImmHistSequenceMap() = default;

  const ImmHistOpcodeSettings &
  getConfigForOpcode(unsigned Opc, const OpcodeCache &OpCC) const {
    assert(Data.count(Opc) && "Opcode was not found in immediate histogram");
    return Data.at(Opc);
  }
};

} // namespace snippy
LLVM_SNIPPY_YAML_DECLARE_MAPPING_TRAITS(snippy::ImmediateHistogramSequence);
} // namespace llvm
