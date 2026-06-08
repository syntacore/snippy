//===-- ProbabilityUtils.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H
#define LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H

#include "snippy/Support/Utils.h"

#include "llvm/ADT/STLExtras.h"

#include <tuple>
#include <vector>

namespace llvm {
namespace snippy {

template <typename T> struct ProbableElement final {
  using elem_type = T;
  T Element;
  double Prob;

  ProbableElement(T Element, double Prob) : Element(Element), Prob(Prob) {}
};

namespace detail {
template <typename MapT>
auto jointProbabilityDistributionImpl(const MapT &Map) {
  using Key = std::tuple<typename MapT::key_type>;
  std::vector<ProbableElement<Key>> Result;
  Result.reserve(Map.size());
  for (const auto &[Key, Prob] : Map)
    Result.emplace_back(std::make_tuple(Key), Prob);

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

  std::vector<ProbableElement<KeyT>> Result;
  Result.reserve(First.size() * RestVec.size());

  for (const auto &[Key1, Prob1] : First) {
    for (const auto &[KeyRest, ProbRest] : RestVec) {
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
// returns std::vector of ProbableElement<std::tuple<Key1T, Key2T, ...>>
template <typename... MapT>
auto jointProbabilityDistribution(const MapT &...Maps) {
  static_assert(detail::are_weight_maps<MapT...>);
  return detail::jointProbabilityDistributionImpl(Maps...);
}

template <typename EnumList> struct EnumMappingFunctions final {
  static_assert(detail::has_static_arr_v<EnumList>);
  using ArrayType = decltype(EnumList::Arr);
  using EnumType = typename ArrayType::value_type;
  static constexpr size_t EnumSize = EnumList::Arr.size();

  // Linear time complexity. Should be used only when Enum argument is
  // constexpr (which is almost always the case).
  static constexpr size_t toIdx(EnumType Enum) {
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
  using Mapping = EnumMappingFunctions<EnumList>;
  static constexpr size_t N = Mapping::EnumSize;

public:
  using mapped_type = DataT;
  using key_type = typename Mapping::EnumType;
  using value_type = std::pair<const key_type, mapped_type>;

private:
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
  MappedArray() : Storage(makeStorage(std::make_index_sequence<N>())) {}

  template <typename... ArgsT,
            typename = std::enable_if_t<sizeof...(ArgsT) == N>>
  explicit MappedArray(ArgsT &&...Args)
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
};

template <typename T> using WeightsArray = MappedArray<T, double>;

template <typename T>
static WeightsArray<T> normalizeWeights(WeightsArray<T> Weights) {
  normalizeValues(make_second_range(Weights));
  return Weights;
}

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_PROBABILITYUTILS_H
