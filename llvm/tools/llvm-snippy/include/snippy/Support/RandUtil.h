//===-- RandUtil.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Error.h"
#include "Utils.h"

#include "snippy/Support/DiagnosticInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <random>
#include <set>
#include <vector>

namespace llvm {
namespace snippy {

namespace details {

template <typename... Ts> constexpr auto makeArray(Ts &&...ts) {
  using CT = std::common_type_t<Ts...>;
  return std::array<CT, sizeof...(Ts)>{std::forward<CT>(ts)...};
}

} // namespace details

// INFO: this snippy implementation of UniformIntDistribution is
// required because std::uniform_int_distribution has different
// implementation(therefore different results) on different OS.
// It is stripped version of std::uniform_int_distribution.
template <typename IntType = int> class UniformIntDistribution final {
  static_assert(std::is_integral<IntType>::value,
                "template argument must be an integral type");

public:
  using result_type = IntType;
  struct ParamType final {
    using DistributionType = UniformIntDistribution<IntType>;

    ParamType() : ParamType(0) {}

    explicit ParamType(IntType Start,
                       IntType End = std::numeric_limits<IntType>::max())
        : Start(Start), End(End) {
      assert(Start <= End);
    }

    result_type a() const { return Start; }

    result_type b() const { return End; }

    friend bool operator==(const ParamType &Lhs, const ParamType &Rhs) {
      return Lhs.Start == Rhs.Start && Lhs.End == Rhs.End;
    }

    friend bool operator!=(const ParamType &Lhs, const ParamType &Rhs) {
      return !(Lhs == Rhs);
    }

  private:
    IntType Start;
    IntType End;
  };

public:
  UniformIntDistribution() : UniformIntDistribution(0) {}

  explicit UniformIntDistribution(
      IntType Start, IntType End = std::numeric_limits<IntType>::max())
      : Param(Start, End) {}

  explicit UniformIntDistribution(const ParamType &Param) : Param(Param) {}

  // Does nothing for the uniform integer distribution.
  void reset() {}

  result_type a() const { return Param.a(); }

  result_type b() const { return Param.b(); }

  ParamType param() const { return Param; }

  void param(const ParamType &ParamIn) { Param = ParamIn; }

  result_type min() const { return this->a(); }

  result_type max() const { return this->b(); }

  template <typename UniformRandomNumberGenerator>
  result_type operator()(UniformRandomNumberGenerator &URNG) {
    return this->operator()(URNG, Param);
  }

  template <typename UniformRandomNumberGenerator>
  result_type operator()(UniformRandomNumberGenerator &URNG,
                         const ParamType &ParamIn) {
    using result_type = typename UniformRandomNumberGenerator::result_type;
    using utype = typename std::make_unsigned<result_type>::type;
    using uctype = typename std::common_type<result_type, utype>::type;

    const uctype URNGMin = URNG.min();
    const uctype URNGMax = URNG.max();
    const uctype URNGRange = URNGMax - URNGMin;
    const uctype URange = uctype(ParamIn.b()) - uctype(ParamIn.a());

    uctype Ret;

    if (URNGRange > URange) {
      // downscaling
      const uctype UERange = URange + 1; // URange can be zero
      const uctype Scaling = URNGRange / UERange;
      const uctype Past = UERange * Scaling;
      do
        Ret = uctype(URNG()) - URNGMin;
      while (Ret >= Past);
      Ret /= Scaling;
    } else if (URNGRange < URange) {
      // upscaling
      /*
        Note that every value in [0, urange]
        can be written uniquely as
        (urngrange + 1) * high + low
        where
        high in [0, urange / (urngrange + 1)]
        and
        low in [0, urngrange].
      */
      uctype Tmp; // wraparound control
      do {
        const uctype UERngRange = URNGRange + 1;
        Tmp =
            (UERngRange * operator()(URNG, ParamType(0, URange / UERngRange)));
        Ret = Tmp + (uctype(URNG()) - URNGMin);
      } while (Ret > URange || Ret < Tmp);
    } else
      Ret = uctype(URNG()) - URNGMin;

    return Ret + ParamIn.a();
  }

  friend bool operator==(const UniformIntDistribution &Lhs,
                         const UniformIntDistribution &Rhs) {
    return Lhs.Param == Rhs.Param;
  }

private:
  ParamType Param;
};

class RandEngine {
  using GeneratorEngineType = std::mt19937_64;
  GeneratorEngineType Engine;

public:
  using ResultType = GeneratorEngineType::result_type;

private:
  ResultType Seed;

  static std::unique_ptr<RandEngine> pimpl;

  RandEngine(ResultType Seed) : Seed{Seed} { Engine.seed(Seed); }

public:
  static void init(ResultType Seed) {
    struct MakeUniqueEnabler : RandEngine {
      MakeUniqueEnabler(ResultType Seed) : RandEngine(Seed) {}
    };
    pimpl = std::make_unique<MakeUniqueEnabler>(Seed);
  }

  static void reInit() { init(pimpl->Seed); }

  template <typename T> static T genInRangeInclusive(T Min, T Max) {
    if (Max < Min)
      snippy::fatal("Invalid usage of genInRangeInclusive");
    UniformIntDistribution<T> Dist(Min, Max);
    return Dist(pimpl->Engine);
  }

  template <typename T> static T genInRangeInclusive(NumericRange<T> Range) {
    auto Min = Range.Min.value_or(0);
    return genInRangeInclusive<T>(Min, Range.Max.value_or(Min));
  }

  template <typename T> static T genInRangeInclusive(T Max) {
    return genInRangeInclusive<T>(0, Max);
  }

  template <typename T> static T genInRangeExclusive(T First, T Last) {
    // without <T> there were deduction problems with unsigned types
    //  smaller than int
    return genInRangeInclusive<T>(First, Last - 1);
  }

  static bool genBool() { return genInRangeInclusive(1); }

  template <typename T> static T genInRangeExclusive(NumericRange<T> Range) {
    if (!Range.Min.has_value())
      Range.Min = 0;
    return genInRangeExclusive<T>(Range.Min.value(),
                                  Range.Max.value_or(Range.Min.value()));
  }

  template <typename T> static T genInRangeExclusive(T Last) {
    return genInRangeExclusive<T>(0, Last);
  }

  static APInt genInRangeInclusive(APInt Last);

  static APInt genAPInt(unsigned Bits);

  template <typename T>
  static Expected<std::vector<T>> genNInRangeInclusive(T Min, T Max, size_t N) {
    if (Max < Min)
      return makeFailure(Errc::LogicError,
                         "Invalid usage of genNInRangeInclusive: Max cannot be "
                         "less than Min.");
    std::vector<T> Vals(N);
    std::generate(Vals.begin(), Vals.end(),
                  [Min, Max]() { return genInRangeInclusive(Min, Max); });
    return Vals;
  }

private:
  // Algorithm is as follows:
  // 1. Select M-1 random values in range [0, N]
  // 2. Add N to them
  // 3. Sort them
  //
  // Number line (M = 9 here):
  // |------|----|----|--|-------|----|--|----|----|
  // 0     x1   x2   x3  x4     x5   x6  x7  x8    N
  // Vector:
  // {x1, x2, x3, x4, x5, x6, x7, x8, N}
  //
  // 4. Compute differences between consecutive values and store
  // them into the same vector
  // (For the first difference std::adjacent_difference just copies x1)
  //
  // |------|----|----|--|-------|----|--|----|----|
  // 0     x1   x2   x3  x4     x5   x6  x7  x8    N
  //      /  \  |                        |  /  \  /
  //  {x1,   x2-x1,      . . . . .      x8-x7, N-x8}
  //
  // The values were sorted, so all differences are non-negative.
  // The sum of all differences is N:
  // x1 + (x2-x1) + ... + (x8-x7) + (N-x8) = N
  template <typename T>
  static void splitNIntoMPartsImpl(SmallVectorImpl<T> &Result, T N, size_t M) {
    Result.reserve(M);
    std::generate_n(std::back_inserter(Result), M - 1,
                    [N]() { return genInRangeInclusive(N); });
    Result.emplace_back(N);

    std::sort(Result.begin(), Result.end());
    std::adjacent_difference(Result.begin(), Result.end(), Result.begin());
  }

  // The algorithm for the weighted case is as follows:
  // 1. Split N into TotalWeight parts using algorithm above
  // 2. For each part in Result, select Weights[i] parts of the split
  // 3. Compute sum of all selected parts
  //
  // The more Weights[i] is, the more parts will be selected and the more
  // likely it is that the sum will be larger.
  template <typename T>
  static void splitNIntoMPartsImpl(SmallVectorImpl<T> &Result, T N,
                                   const SmallVectorImpl<T> &Weights) {
    static_assert(std::is_integral_v<T>, "T must be an integer type.");
    // Split N into TotalWeight parts. Then combine Weights[i] parts of the
    // split into Result[i].
    T TotalWeight = std::accumulate(Weights.begin(), Weights.end(), T{0});
    assert(TotalWeight != 0 && "Cannot split into 0 parts.");

    SmallVector<T> SparseResult;
    splitNIntoMPartsImpl(SparseResult, N, TotalWeight);

    Result.resize(Weights.size());
    auto SparseResIter = SparseResult.begin();
    for (auto [ResVal, Weight] : zip_equal(Result, Weights)) {
      ResVal = std::accumulate(SparseResIter, SparseResIter + Weight, T{0});
      std::advance(SparseResIter, Weight);
    }
  }

  // Handling Baseline and Uniformity
  template <typename T>
  static void splitNIntoMPartsImpl(SmallVectorImpl<T> &Result, T N, size_t M,
                                   T Baseline, double Uniformity) {
    Baseline = std::max(Baseline, static_cast<T>(Uniformity * N / M));
    if (Baseline == 0) {
      splitNIntoMPartsImpl(Result, N, M);
      return;
    }
    N -= M * Baseline;
    splitNIntoMPartsImpl(Result, N, M);
    for (auto &V : Result)
      V += Baseline;
  }

  // Handling Baseline and Uniformity for the weighted case
  template <typename T>
  static void splitNIntoMPartsImpl(SmallVectorImpl<T> &Result, T N,
                                   const SmallVectorImpl<T> &Weights,
                                   T Baseline, double Uniformity) {
    Baseline =
        std::max(Baseline, static_cast<T>(Uniformity * N / Weights.size()));
    if (Baseline == 0) {
      splitNIntoMPartsImpl(Result, N, Weights);
      return;
    }
    N -= Weights.size() * Baseline;
    splitNIntoMPartsImpl(Result, N, Weights);
    for (auto &V : Result)
      V += Baseline;
  }

  // Handling Alignment
  template <typename T, typename MType>
  static void splitNIntoMPartsImpl(SmallVectorImpl<T> &Result, T N,
                                   const MType &M, T Baseline,
                                   double Uniformity, T Alignment) {
    if (Alignment == 1) {
      splitNIntoMPartsImpl(Result, N, M, Baseline, Uniformity);
      return;
    }
    N /= Alignment;
    Baseline = llvm::alignTo(Baseline, Alignment) / Alignment;

    splitNIntoMPartsImpl(Result, N, M, Baseline, Uniformity);
    for (auto &V : Result)
      V *= Alignment;
  }

public:
  // e.g. splitNIntoMParts(10, 3) -> {4, 3, 3}
  // Each part is at least Baseline. Uniformity [0-1] pushes the baseline to
  // the average. Each part is divisible by Alignment. Note that uniformity 1.0
  // does not guarantee that all parts are 1 Alignment apart or closer.
  template <typename T>
  static void splitNIntoMParts(SmallVectorImpl<T> &Result, T N, size_t M,
                               T Baseline = static_cast<T>(0),
                               double Uniformity = 0.0,
                               T Alignment = static_cast<T>(1)) {
    static_assert(std::is_integral_v<T>, "T must be an integer type.");

    assert(M != 0 && "Cannot split into 0 parts.");
    assert(N >= M * Baseline && "Cannot satisfy the baseline.");
    assert(N >= M * llvm::alignTo(Baseline, Alignment) &&
           "Cannot satisfy the baseline with alignment.");
    assert(Uniformity >= 0.0 && Uniformity <= 1.0 &&
           "Uniformity must be between 0 and 1.");
    assert(N % Alignment == 0 && "N must be divisible by Alignment.");

    if (M == 1) {
      Result.push_back(N);
      return;
    }
    splitNIntoMPartsImpl(Result, N, M, Baseline, Uniformity, Alignment);
  }

  // Result is exactly as in the unweighted version if Weights is {1, 1, ... 1}.
  // Prefer using smaller weights, e.g. Weights = {1, 2, 1} instead
  // of {100, 200, 100}, as they produce the same result.
  template <typename T>
  static void splitNIntoMPartsWeighted(SmallVectorImpl<T> &Result, T N,
                                       const SmallVectorImpl<T> &Weights,
                                       T Baseline = static_cast<T>(0),
                                       double Uniformity = 0.0,
                                       T Alignment = static_cast<T>(1)) {
    static_assert(std::is_integral_v<T>, "T must be an integer type.");

    const auto M = Weights.size();
    assert(M != 0 && "Cannot split into 0 parts.");
    assert(N >= M * Baseline && "Cannot satisfy the baseline.");
    assert(N >= M * llvm::alignTo(Baseline, Alignment) &&
           "Cannot satisfy the baseline with alignment.");
    assert(Uniformity >= 0.0 && Uniformity <= 1.0 &&
           "Uniformity must be between 0 and 1.");
    assert(N % Alignment == 0 && "N must be divisible by Alignment.");
    assert(all_of(Weights, [](T V) { return V >= 0; }) &&
           "All weights must be non-negative.");

    if (M == 1) {
      Result.push_back(N);
      return;
    }
    splitNIntoMPartsImpl(Result, N, Weights, Baseline, Uniformity, Alignment);
  }

  template <typename T>
  static Expected<std::vector<T>> genNUniqInInterval(T Min, T Max, size_t N) {
    static_assert(std::is_integral_v<T>, "T must be an integer type.");
    if (Max < Min)
      return makeFailure(
          Errc::LogicError,
          "Invalid usage of genNUniqInInterval: Max cannot be less than Min.");
    if (N > Max - Min + 1)
      return makeFailure(Errc::LogicError,
                         "Invalid usage of genNUniqInInterval: impossible to "
                         "generate this many unique values.");
    std::vector<T> Vals(Max - Min + 1);
    std::iota(Vals.begin(), Vals.end(), Min);
    shuffle(Vals.begin(), Vals.end());
    Vals.resize(N);
    return Vals;
  }

  template <typename T, typename Pred>
  static Expected<std::vector<T>> genNUniqInInterval(T Min, T Max, size_t N,
                                                     Pred FilterOut) {
    static_assert(std::is_invocable_r_v<bool, Pred, T>,
                  "Filter must be callable with T and return bool");
    if (Max < Min)
      return makeFailure(
          Errc::LogicError,
          "Invalid usage of genNUniqInInterval: Max cannot be less than Min.");
    if (Max - Min + 1 < static_cast<T>(N))
      return makeFailure(
          Errc::LogicError,
          "Invalid usage of genNUniqInInterval: The interval has less unique "
          "values than was requested to generate.");

    std::vector<T> Vals;
    const auto IotaRange = llvm::iota_range<T>(Min, Max, /*Inclusive*/ true);
    const auto FilteredRange =
        llvm::make_filter_range(IotaRange, std::not_fn(FilterOut));
    llvm::copy(FilteredRange, std::back_inserter(Vals));

    if (Vals.size() < N)
      return makeFailure(Errc::NoElements,
                         "Error in genNUniqInInterval: The filtered "
                         "interval has less unique "
                         "values than was requested .");
    shuffle(Vals.begin(), Vals.end());
    Vals.resize(N);
    return Vals;
  }

  template <typename T, typename Pred>
  static size_t countIndicesPassingFilter(T Min, T Max, Pred FilterOut) {
    static_assert(std::is_invocable_r_v<bool, Pred, T>,
                  "Filter must be callable with T and return bool");
    assert(Max >= Min);

    const auto IotaRange = llvm::iota_range<T>(Min, Max, /*Inclusive*/ true);
    const auto FilteredRange =
        llvm::make_filter_range(IotaRange, std::not_fn(FilterOut));
    return std::distance(FilteredRange.begin(), FilteredRange.end());
  }

  // Doesn't require container to be random access,
  // still O(1) for random access containers
  template <typename ContainerT>
  static auto
  selectFromContainer(ContainerT &Container) -> decltype(*Container.begin()) {
    size_t ContainerSize = std::distance(Container.begin(), Container.end());
    assert(ContainerSize > 0);

    auto SelectedIdx = genInRangeExclusive<unsigned>(0, ContainerSize);
    auto SelectedPos = Container.begin();
    std::advance(SelectedPos, SelectedIdx);
    return *SelectedPos;
  }

  // Doesn't require container to be random access,
  // still O(1) for random access containers
  template <typename ContainerT>
  static auto
  selectFromContainerWeighted(ContainerT &Container,
                              std::discrete_distribution<unsigned> &DD)
      -> decltype(*Container.begin()) {
    size_t ContainerSize = std::distance(Container.begin(), Container.end());
    assert(ContainerSize > 0);
    assert(DD.probabilities().size() == ContainerSize);

    auto SelectedIdx = DD(pimpl->Engine);
    auto SelectedPos = Container.begin();
    std::advance(SelectedPos, SelectedIdx);
    return *SelectedPos;
  }

  // Doesn't require container to be random access
  template <typename ContainerT, typename Pred>
  static auto selectFromContainerFiltered(ContainerT &Container, Pred FilterOut)
      -> Expected<decltype(*Container.begin())> {
    static_assert(
        std::is_invocable_r_v<bool, Pred, const decltype(*Container.begin())>,
        "Filter must be callable with const T and return bool");
    auto Candidates =
        llvm::make_filter_range(Container, std::not_fn(FilterOut));

    if (Candidates.empty())
      return makeFailure(Errc::NoElements,
                         "Error in selectFromContainerFiltered: No candidates "
                         "left after filtering.");
    return selectFromContainer(Candidates);
  }

  static auto &engine() { return pimpl->Engine; }

  // INFO: this snippy implementation of shuffle is required because
  // std::shuffle uses std::uniform_int_distribution that has different
  // implementation(therefore different results) on different OS.
  // It is stripped version of std::shuffle.
  template <typename IterT> static void shuffle(IterT First, IterT Last) {
    if (First == Last)
      return;

    using DistanceT = typename std::iterator_traits<IterT>::difference_type;

    using UniformDistT = typename std::make_unsigned<DistanceT>::type;
    using DistrT = UniformIntDistribution<UniformDistT>;
    using ParamT = typename DistrT::ParamType;

    using CommonUniformT =
        typename std::common_type<typename GeneratorEngineType::result_type,
                                  UniformDistT>::type;

    const CommonUniformT URngRange = engine().max() - engine().min();
    const CommonUniformT URange = CommonUniformT(Last - First);

    if (URngRange / URange >= URange)
    // I.e. (__urngrange >= __urange * __urange) but without wrap issues.
    {
      IterT It = First + 1;

      // Since we know the range isn't empty, an even number of elements
      // means an uneven number of elements /to swap/, in which case we
      // do the first one up front:

      if ((URange % 2) == 0) {
        DistrT D{0, 1};
        std::iter_swap(It++, First + D(engine()));
      }

      // Now we know that __last - __i is even, so we do the rest in pairs,
      // using a single distribution invocation to produce swap positions
      // for two successive elements at a time:

      auto GenTwoUniformInts = [](CommonUniformT B0, CommonUniformT B1) {
        CommonUniformT X =
            UniformIntDistribution<CommonUniformT>{0, (B0 * B1) - 1}(engine());
        return std::make_pair(X / B1, X % B1);
      };

      while (It != Last) {
        const CommonUniformT SwapRange = CommonUniformT(It - First) + 1;

        const std::pair<CommonUniformT, CommonUniformT> PosPos =
            GenTwoUniformInts(SwapRange, SwapRange + 1);

        std::iter_swap(It++, First + PosPos.first);
        std::iter_swap(It++, First + PosPos.second);
      }

      return;
    }

    DistrT D;

    for (IterT It = First + 1; It != Last; ++It)
      std::iter_swap(It, First + D(engine(), ParamT(0, It - First)));
  }
};

template <typename T> class RandomEliminationGenerator {
public:
  // This generator produces sequences of numbers
  // in range of [From, To). There are no duplicate
  // numbers in any sequence. Each next number in sequence
  // is randomly choosen from remaining available numbers.
  // If none such numbers left, overloaded operator bool return
  // false and next() must not be called.
  // Generator is optimized for relativly small sequences of
  // wide range (To - From >> n of next() calls)
  RandomEliminationGenerator(T From, T To)
      : From{From}, To{To}, Size{To - From} {}

  operator bool() const { return Size > 0u; }

  T next() {
    assert(Size != 0u);
    // Algorithm works as follows:
    // Assume range from 1 to 9 where elements
    // 1, 3, 4, 5, 8, 9 have been picked:
    //
    // rid:   1       2 3
    // rem:   2       6 7
    // pic: 1   3 4 5     8 9
    // ran: 1 2 3 4 5 6 7 8 9
    //
    // Remaining elements(rem) are indexed(rid) starting
    // with first element of range(ran). One index from rid is
    // randomly choosen. Convertion from index to element is done
    // by scanning picked elements set(pic) one by one and
    // shifting index each time it is greater or equal to
    // element in set.
    // E.G. Assume index R = 2 is picked. Then scanning
    // will be done like this:
    // R = 2 >= 1 -> R++;
    // R = 3 >= 3 -> R++;
    // R = 4 >= 4 -> R++;
    // R = 5 >= 5 -> R++;
    // R = 6 <  8 -> done;
    auto R = From + RandEngine::genInRangeExclusive(Size);
    for (auto P : Picked) {
      if (R < P)
        break;
      R++;
    }
    auto [_, Inserted] = Picked.insert(R);
    assert(Inserted);
    Size--;
    return R;
  }

private:
  T From, To, Size;
  std::set<T> Picked;
};

template <typename T>
void uniformlyFill(MutableArrayRef<T> Out, T Low, T High) {
  UniformIntDistribution<T> Dist(Low, High);
  auto &Engine = RandEngine::engine();
  std::generate(Out.begin(), Out.end(), [&] { return Dist(Engine); });
}

template <typename... Ts> auto selectFrom(Ts... args) {
  auto Items = details::makeArray(std::forward<Ts>(args)...);
  return Items[RandEngine::genInRangeInclusive(Items.size() - 1)];
}

} // namespace snippy
} // namespace llvm
