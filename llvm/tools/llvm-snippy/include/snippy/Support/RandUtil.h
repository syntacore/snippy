//===-- RandUtil.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Error.h"

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

  template <typename T> static T genInInterval(T Min, T Max) {
    if (Max < Min)
      report_fatal_error("Invalid usage of genInInterval");
    std::uniform_int_distribution<T> Dist(Min, Max);
    return Dist(pimpl->Engine);
  }

  template <typename T> static T genInInterval(T Max) {
    return genInInterval<T>(0, Max);
  }

  template <typename T> static T genInRange(T First, T Last) {
    // without <T> there were deduction problems with unsigned types
    //  smaller than int
    return genInInterval<T>(First, Last - 1);
  }

  template <typename T> static T genInRange(T Last) {
    return genInRange<T>(0, Last);
  }

  static APInt genInInterval(APInt Last);

  static APInt genAPInt(unsigned Bits);

  template <typename T>
  static Expected<std::vector<T>> genNUniqInInterval(T Min, T Max, size_t N) {
    if (Max < Min)
      return make_error<Failure>(
          "Invalid usage of genNUniqInInterval: Max cannot be less than Min.");
    std::vector<T> Vals(Max - Min + 1);
    std::iota(Vals.begin(), Vals.end(), Min);
    std::shuffle(Vals.begin(), Vals.end(), pimpl->Engine);
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
    std::shuffle(Vals.begin(), Vals.end(), pimpl->Engine);
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
    auto R = From + RandEngine::genInRange(Size);
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
  std::uniform_int_distribution<T> Dist(Low, High);
  auto &Engine = RandEngine::engine();
  std::generate(Out.begin(), Out.end(), [&] { return Dist(Engine); });
}

template <typename... Ts> auto selectFrom(Ts... args) {
  auto Items = details::makeArray(std::forward<Ts>(args)...);
  return Items[RandEngine::genInInterval(Items.size() - 1)];
}

} // namespace snippy
} // namespace llvm
