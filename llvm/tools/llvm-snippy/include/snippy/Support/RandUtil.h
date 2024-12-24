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
  static Expected<std::vector<T>> genNUniqInInterval(T Min, T Max, size_t N) {
    if (Max < Min)
      return make_error<Failure>(
          "Invalid usage of genNUniqInInterval: Max cannot be less than Min.");
    std::vector<T> Vals(Max - Min + 1);
    std::iota(Vals.begin(), Vals.end(), Min);
    shuffle(Vals.begin(), Vals.end());
    Vals.resize(N);
    return Vals;
  }

  template <typename T, typename Pred>
  static Expected<size_t> countUniqInInterval(T Min, T Max, Pred Filter) {
    if (Max < Min)
      return make_error<Failure>(
          "Invalid usage of countUniqInInterval: Max cannot be less than Min.");

    T K = Min;
    auto G = [&K, Max, &Filter]() {
      for (; K <= Max && Filter(K); ++K)
        ;
      return K++;
    };
    size_t Ret = 0u;
    for (auto Next = G(); Next <= Max; Next = G())
      ++Ret;
    return Ret;
  }

  template <typename T, typename Pred>
  static Expected<std::vector<T>> genNUniqInInterval(T Min, T Max, size_t N,
                                                     Pred Filter) {
    if (Max < Min)
      return make_error<Failure>(
          "Invalid usage of genNUniqInInterval: Max cannot be less than Min.");
    if (Max - Min + 1 < N)
      return make_error<Failure>(
          "Invalid usage of genNUniqInInterval: The interval has less unique "
          "values than was requested to generate.");

    T K = Min;
    auto G = [&K, Max, &Filter]() {
      for (; K <= Max && Filter(K); ++K)
        ;
      return K++;
    };
    std::vector<T> Vals;
    for (auto Next = G(); Next <= Max; Next = G())
      Vals.push_back(Next);
    if (Vals.size() < N)
      return make_error<Failure>(
          "Invalid usage of genNUniqInInterval: The interval has less unique "
          "values than was requested.");
    shuffle(Vals.begin(), Vals.end());
    Vals.resize(N);
    return Vals;
  }

  template <size_t N, typename T, typename Pred>
  static Expected<std::array<T, N>> genNUniqInInterval(T Min, T Max,
                                                       Pred Filter) {
    std::array<T, N> Result;
    auto IntermResults = genNUniqInInterval(Min, Max, N, std::move(Filter));
    if (!IntermResults)
      return IntermResults.takeError();
    assert(IntermResults->size() == Result.size());
    std::copy(IntermResults->begin(), IntermResults->end(), Result.begin());
    return Result;
  }

  /// Select random element from container that doesn't meet requirements of
  /// random access iterator.
  template <typename ContainerT>
  static decltype(auto)
  selectFromContainer(const ContainerT &Container,
                      std::discrete_distribution<unsigned> &DD) {
    auto Size = Container.size();
    auto SelectedIdx = DD(pimpl->Engine);
    assert(SelectedIdx < Size && "Selected element in set is out of range");
    auto SelectedPos = Container.begin();
    std::advance(SelectedPos, SelectedIdx);
    return *SelectedPos;
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
