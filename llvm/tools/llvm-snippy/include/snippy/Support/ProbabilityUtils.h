//===-- ProbabilityUtils.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H
#define LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H

#include "snippy/Support/RandUtil.h"
#include "snippy/Support/Utils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <random>
#include <tuple>
#include <type_traits>
#include <vector>

namespace llvm {
namespace snippy {

namespace detail {

// Check existence of T::print(llvm::raw_ostream&)
template <typename T, typename = void>
struct has_print_with_ostream : std::false_type {};

template <typename T>
struct has_print_with_ostream<T, std::void_t<decltype(std::declval<T>().print(
                                     std::declval<llvm::raw_ostream &>()))>>
    : std::true_type {};

template <typename T>
constexpr bool has_print_with_ostream_v = has_print_with_ostream<T>::value;

// Check existence of operator<<(llvm::raw_ostream&, const T&)
template <typename T, typename = void>
struct has_output_operator : std::false_type {};

template <typename T>
struct has_output_operator<
    T, std::void_t<decltype(std::declval<llvm::raw_ostream &>()
                            << std::declval<const T &>())>> : std::true_type {};

template <typename T>
constexpr bool has_output_operator_v = has_output_operator<T>::value;

template <typename T>
constexpr bool is_printable_v =
    has_print_with_ostream_v<T> || has_output_operator_v<T>;

// Print a container of pairs {key_type, prob_type} using a custom function
// that takes a const key_type& and returns a printable
template <typename Container, typename Func>
void print(const Container &C, raw_ostream &OS, Func &&StrFunc) {
  using key_type = typename Container::key_type;
  static_assert(std::is_invocable_v<Func, const key_type &>,
                "StrFunc must be invocable with const key_type&");

  using PrintableT = std::invoke_result_t<Func, const key_type &>;
  static_assert(has_output_operator_v<PrintableT>,
                "The result of StrFunc must be streamable to raw_ostream");

  for (const auto &[Element, Prob] : C) {
    OS << "[" << std::invoke(std::forward<Func>(StrFunc), Element)
       << formatv(", {0:P}]\n", Prob);
  }
  OS << "\n";
}

} // namespace detail

template <typename T> struct ProbableElement final {
  static_assert(!std::is_reference_v<T>, "T must not be a reference type");

  using elem_type = T;
  using prob_type = double;

  elem_type Element;
  prob_type Prob;

  ProbableElement(elem_type Element, prob_type Prob)
      : Element(std::move(Element)), Prob(Prob) {}

  ProbableElement(std::pair<elem_type, prob_type> Pair)
      : ProbableElement(std::move(Pair.first), Pair.second) {}
};

// A wrapper around SmallVector<ProbableElement<T>> with some helper functions
// and definitions to use in other classes and functions.
template <typename T>
struct ProbableItems final : SmallVector<ProbableElement<T>> {
  using prob_type = typename ProbableElement<T>::prob_type;
  using key_type = typename ProbableElement<T>::elem_type;
  using mapped_type = prob_type;
  using value_type = ProbableElement<T>;

  using SmallVector<ProbableElement<T>>::SmallVector;
  using SmallVector<ProbableElement<T>>::begin;
  using SmallVector<ProbableElement<T>>::end;

  ProbableItems(const std::vector<std::pair<key_type, prob_type>> &Items)
      : ProbableItems(Items.begin(), Items.end()) {}

  auto getItemsRange() const {
    return map_range(*this,
                     [](const ProbableElement<T> &E) { return E.Element; });
  }

  auto getProbsRange() const {
    return map_range(*this, [](const ProbableElement<T> &E) { return E.Prob; });
  }

  prob_type getTotalProb() const {
    return std::accumulate(
        begin(), end(), 0.0,
        [](prob_type Total, const auto &Item) { return Total + Item.Prob; });
  }

  void normalizeProbs() {
    prob_type TotalWeight = getTotalProb();
    assert(!isZero(TotalWeight));
    for (auto &Item : *this)
      Item.Prob /= TotalWeight;

    assert(checkSumOfProbabilities());
  }

  template <typename U = T>
  std::enable_if_t<detail::is_printable_v<U>> print(raw_ostream &OS) const {
    for (const auto &[Element, Prob] : *this) {
      OS << "[";
      if constexpr (detail::has_print_with_ostream_v<T>)
        Element.print(OS);
      else if constexpr (detail::has_output_operator_v<T>)
        OS << Element;
      else
        llvm_unreachable("Cannot print this type");

      OS << formatv(", {0:P}]\n", Prob);
    }
    OS << "\n";
  }

  // Print with a custom function that takes a const T& and returns a printable
  template <typename Func> void print(raw_ostream &OS, Func &&StrFunc) const {
    detail::print(*this, OS, std::forward<Func>(StrFunc));
  }

  // Check if the probabilities add up to 1
  bool checkSumOfProbabilities() const {
    // The floating point error scales with the number of items, so we multiply
    // by size(). The BigNumber is a magic number and it allows for some slack.
    constexpr auto BigNumber = 1000000;
    auto Tolerance =
        this->size() * BigNumber * std::numeric_limits<prob_type>::epsilon();
    assert(Tolerance < 0.001);
    return std::abs(getTotalProb() - 1.0) < Tolerance;
  }

  bool contains(const T &Element) const {
    return llvm::is_contained(
        map_range(*this, [](const auto &Item) { return Item.Element; }),
        Element);
  }

  // Returns a pointer to the probability of the *first* element with Key
  // or nullptr if there is no such element. Note that there is no guarantee
  // that the element is unique.
  prob_type *getProb(const key_type &Key) {
    auto It =
        std::find_if(this->begin(), this->end(),
                     [&](const value_type &V) { return V.Element == Key; });
    if (It == this->end())
      return nullptr;
    return &(It->Prob);
  }

  // Get a reference to the probability by key (first occurrence), or add a new
  // element if not found. Similar to std::map::operator[].
  prob_type &getProbOrEmplace(const key_type &Key) {
    if (auto *ProbPtr = getProb(Key))
      return *ProbPtr;
    this->emplace_back(Key, prob_type{});
    return this->back().Prob;
  }
};

namespace detail {
template <typename MapT>
auto jointProbabilityDistributionImpl(const MapT &Map) {
  using Key = std::tuple<typename MapT::key_type>;
  ProbableItems<Key> Result;
  Result.reserve(Map.size());
  for (const auto &[Key, Prob] : Map) {
    if (!isZero(Prob))
      Result.emplace_back(std::make_tuple(Key), Prob);
  }
  return Result;
}

template <typename FirstMap, typename... RestMaps>
auto jointProbabilityDistributionImpl(const FirstMap &First,
                                      const RestMaps &...Rest) {
  auto RestVec = jointProbabilityDistributionImpl(Rest...);
  using RestProbElem = typename decltype(RestVec)::value_type;
  using RestKey = typename RestProbElem::elem_type;
  using KeyT = decltype(std::tuple_cat(
      std::declval<std::tuple<typename FirstMap::key_type>>(),
      std::declval<RestKey>()));

  ProbableItems<KeyT> Result;
  Result.reserve(First.size() * RestVec.size());

  for (const auto &[Key1, Prob1] : First) {
    if (isZero(Prob1))
      continue;
    for (const auto &[KeyRest, ProbRest] : RestVec) {
      // RestVec comes from jointProbabilityDistributionImpl, so all probs
      // in it must be non-zero
      assert(!isZero(ProbRest));
      KeyT NewKey = std::tuple_cat(std::make_tuple(Key1), KeyRest);
      Result.emplace_back(std::move(NewKey), Prob1 * ProbRest);
    }
  }
  return Result;
}

template <typename... MapT>
constexpr bool are_weight_maps =
    (std::is_convertible_v<typename MapT::mapped_type, double> && ...);

template <typename, typename = void> struct has_static_arr : std::false_type {};

template <typename T>
struct has_static_arr<T, std::void_t<decltype(T::Arr)>> : std::true_type {};

template <typename T>
constexpr bool has_static_arr_v = has_static_arr<T>::value;

} // namespace detail

// Takes a list of map-like objects (KeyT -> double) and
// returns ProbableItems<std::tuple<Key1T, Key2T, ...>>.
// Erases combinations with zero probability.
template <typename... MapT>
[[nodiscard]] auto jointProbabilityDistribution(const MapT &...Maps) {
  static_assert(detail::are_weight_maps<MapT...>);
  return detail::jointProbabilityDistributionImpl(Maps...);
}

template <typename EnumList> struct EnumMappingFunctions final {
  static_assert(detail::has_static_arr_v<EnumList>);
  using ArrayType = decltype(EnumList::Arr);
  using EnumType = typename ArrayType::value_type;
  static constexpr size_t EnumSize = EnumList::Arr.size();

  // Be aware of the linear time complexity.
  static size_t toIdx(EnumType Enum) {
    auto It = llvm::find(EnumList::Arr, Enum);
    if (It != EnumList::Arr.end())
      return std::distance(EnumList::Arr.begin(), It);
    llvm_unreachable("Unexpected enum representation");
  }

  static constexpr EnumType toEnum(size_t Idx) {
    if (Idx < EnumSize)
      return EnumList::Arr[Idx];
    llvm_unreachable("Unexpected Idx for enum");
  }
};

// Similar in spirit to `llvm::IndexedMap`, but stores array of
// pairs<key_type, mapped_type> for easier iteration and automatically creates
// mapping function for enums. This class requires EnumList type, which must
// contain `static constexpr std::array Arr` with all possible enum values.
template <typename EnumList, typename DataT> class MappedArray final {
public:
  using Mapping = EnumMappingFunctions<EnumList>;
  using mapped_type = DataT;
  using key_type = typename Mapping::EnumType;
  using value_type = std::pair<const key_type, mapped_type>;

private:
  static constexpr size_t N = Mapping::EnumSize;
  using StorageType = std::array<value_type, N>;
  StorageType Storage;

  template <std::size_t... I>
  static constexpr auto makeStorage(std::index_sequence<I...>) {
    return StorageType{value_type{Mapping::toEnum(I), mapped_type{}}...};
  }

  template <std::size_t... I, typename... ArgsT>
  static constexpr StorageType makeStorageWithValues(std::index_sequence<I...>,
                                                     ArgsT &&...Args) {
    static_assert(sizeof...(ArgsT) == N);
    static_assert(sizeof...(I) == N);
    return StorageType{
        value_type(Mapping::toEnum(I), std::forward<ArgsT>(Args))...};
  }

public:
  // Default-construct all data elements
  MappedArray() : Storage(makeStorage(std::make_index_sequence<N>())) {}

  // Construct all data elements with the given values
  template <typename... ArgsT,
            typename = std::enable_if_t<sizeof...(ArgsT) == N>>
  MappedArray(ArgsT &&...Args)
      : Storage(makeStorageWithValues(std::make_index_sequence<N>(),
                                      std::forward<ArgsT>(Args)...)) {}
  MappedArray(const MappedArray &) = default;
  MappedArray(MappedArray &&) = default;

  MappedArray &operator=(const MappedArray &Other) {
    if (this != &Other) {
      for (size_t I = 0; I < N; ++I) {
        // .first must already be the same, since it's const and
        // depends only on the EnumList template type
        assert(Storage[I].first == Other.Storage[I].first);
        Storage[I].second = Other.Storage[I].second;
      }
    }
    return *this;
  }

  MappedArray &operator=(MappedArray &&Other) {
    if (this != &Other) {
      for (size_t I = 0; I < N; ++I) {
        // .first must already be the same, since it's const and
        // depends only on the EnumList template type
        assert(Storage[I].first == Other.Storage[I].first);
        Storage[I].second = std::move(Other.Storage[I].second);
      }
    }
    return *this;
  }

  mapped_type &atIdx(size_t Idx) {
    assert(Idx < N);
    return Storage[Idx].second;
  }
  const mapped_type &atIdx(size_t Idx) const {
    assert(Idx < N);
    return Storage[Idx].second;
  }

  // Avoid using when Key is not constexpr
  mapped_type &operator[](key_type Key) { return atIdx(Mapping::toIdx(Key)); }
  // Avoid using when Key is not constexpr
  const mapped_type &operator[](key_type Key) const {
    return atIdx(Mapping::toIdx(Key));
  }

  using iterator = typename StorageType::iterator;
  using const_iterator = typename StorageType::const_iterator;

  const_iterator begin() const { return Storage.begin(); }
  const_iterator end() const { return Storage.end(); }
  iterator begin() { return Storage.begin(); }
  iterator end() { return Storage.end(); }

  static constexpr size_t size() { return N; }

  [[nodiscard]] std::array<mapped_type, N> toArray() const {
    std::array<mapped_type, N> Result;
    copy(make_second_range(Storage), Result.begin());
    return Result;
  }

  // Print with a custom function that takes a key_type and returns a
  // printable. (key_type is typically an enum)
  template <typename Func> void print(raw_ostream &OS, Func &&StrFunc) const {
    detail::print(*this, OS, std::forward<Func>(StrFunc));
  }
};

template <typename T> using WeightsArray = MappedArray<T, double>;

template <typename T>
static WeightsArray<T> normalizeWeights(WeightsArray<T> Weights) {
  normalizeValues(make_second_range(Weights));
  return Weights;
}

template <typename T> struct DiscreteItemGenerator {
  SmallVector<T> Items;
  mutable std::discrete_distribution<unsigned> Dist;

  DiscreteItemGenerator(const ProbableItems<T> &ProbableItems) {
    llvm::copy(ProbableItems.getItemsRange(), std::back_inserter(Items));
    auto Probs = ProbableItems.getProbsRange();
    Dist = std::discrete_distribution<unsigned>(Probs.begin(), Probs.end());
  }

  DiscreteItemGenerator(ProbableItems<T> &&ProbableItems) {
    for (auto &&Item : ProbableItems)
      Items.emplace_back(std::move(Item.Element));
    auto Probs = ProbableItems.getProbsRange();
    Dist = std::discrete_distribution<unsigned>(Probs.begin(), Probs.end());
  }

  // When T is enum type we can use special EnumList type and construct from
  // WeightsArray.
  template <typename EnumList>
  DiscreteItemGenerator(const WeightsArray<EnumList> &Weights) {
    static_assert(std::is_enum_v<T>);
    static_assert(
        std::is_same_v<
            typename std::remove_cv_t<decltype(EnumList::Arr)>::value_type, T>);
    for (auto &&Item : EnumList::Arr)
      Items.emplace_back(Item);
    auto WeightsArr = Weights.toArray();
    Dist = std::discrete_distribution<unsigned>(WeightsArr.begin(),
                                                WeightsArr.end());
  }

  const T &generate() const {
    return RandEngine::selectFromContainerWeighted(Items, Dist);
  }

  template <typename Predicate>
  Expected<const T &> generateIf(Predicate Pred) const {
    return RandEngine::selectFromContainerWeightedFiltered(
        Items, Dist.probabilities(), std::not_fn(Pred));
  }

  [[nodiscard]] ProbableItems<T> toProbableItems() const {
    auto Probs = Dist.probabilities();
    ProbableItems<T> Result;
    Result.reserve(Items.size());
    for (const auto &[Item, Prob] : zip_equal(Items, Probs))
      Result.emplace_back(Item, Prob);
    return Result;
  }
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H
