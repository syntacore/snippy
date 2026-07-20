//===-- YAMLProbableItems.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLPROBABLEITEMS_H
#define LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLPROBABLEITEMS_H

#include "snippy/Support/ProbabilityUtils.h"
#include "snippy/Support/YAMLTuple.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/YAMLTraits.h"

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace llvm {
namespace snippy {

// Detects std::string T::validate() const
template <typename T, typename Enable = void>
struct has_member_validate : std::false_type {};

template <typename T>
struct has_member_validate<T,
                           std::enable_if_t<std::is_invocable_r_v<
                               std::string, decltype(&T::validate), const T &>>>
    : std::true_type {};

namespace detail {

// SFINAE way of optionally providing `Name` field to YAMLTupleTraits
template <typename T, typename Enable = void> struct ProbableElementNameBase {};

template <typename T>
struct ProbableElementNameBase<T, std::void_t<decltype(T::Label)>> {
  static constexpr auto Name = T::Label;
};

} // namespace detail

// Maps ProbableElement<T> as [element, weight] tuple. Validates weight and
// the element (if T has `validate` method).
//
// Hustle with validate() is done only because sometimes it's the only way.
// In some cases validation errors should instead be produced by yamlize().
//
// For cases like:
//   1) [[10, 20], 1.0] ->  ProbableElement<MyStruct>
//   2) [A: B, 1.0] ->  ProbableElement<MyMap>
// you SHOULD NOT use validate() here because validation can be declared
// directly in YAMLTupleTraits<T> or MappingTraits<T> respectively.
//
// However, there're cases like:
//   1) [[1, 2, 12], 1.0] ->  ProbableElement<std::vector<int>>
//     - SequenceTraits simply doesn't have validate().
//   2) [A, 1.0] ->  ProbableElement<MyScalar>
//     - ScalarTraits forces validate() to return StringRef, meaning that
//       error message must be compile-time constant.
//       (alternatively, you can use LLVM_SNIPPY_YAML_DECLARE_SCALAR_TRAITS_NG)
template <typename T>
struct YAMLTupleTraits<ProbableElement<T>>
    : detail::ProbableElementNameBase<T> {
  static auto members(ProbableElement<T> &E) {
    return std::tie(E.Element, E.Prob);
  }

  static std::string validate(const ProbableElement<T> &E) {
    if (!isValidWeight(E.Prob))
      return detail::labelMsg<ProbableElement<T>>(
          "weights must be non-negative!");
    if constexpr (has_member_validate<T>::value)
      return E.Element.validate();
    return {};
  }
};

namespace detail {

// Negative weights are rejected per-element, so only all-zeroes is left.
template <typename T>
std::string validateProbableItems(const ProbableItems<T> &Items) {
  if (any_of(Items, [](const ProbableElement<T> &E) { return E.Prob > 0.0; }))
    return {};
  return labelMsg<ProbableElement<T>>("at least one weight must be positive!");
}

} // namespace detail
} // namespace snippy
} // namespace llvm

// Enables `ProbableItems<_elem_type>` as a sequence of [element, weight]
// entries with a whole-list weight check.
#define LLVM_SNIPPY_YAML_IS_PROBABLE_ITEMS(_elem_type)                         \
  LLVM_SNIPPY_YAML_IS_TUPLE(snippy::ProbableElement<_elem_type>)               \
  namespace yaml {                                                             \
  template <>                                                                  \
  struct SequenceElementTraits<snippy::ProbableElement<_elem_type>> {          \
    static const bool flow = false;                                            \
  };                                                                           \
  void yamlize(yaml::IO &Io, snippy::ProbableItems<_elem_type> &Items, bool,   \
               EmptyContext &Ctx) {                                            \
    static_assert(                                                             \
        missingTraits<snippy::ProbableItems<_elem_type>, EmptyContext>::value, \
        "ProbableItems types must not define other YAML traits");              \
    yamlize(                                                                   \
        Io,                                                                    \
        static_cast<SmallVectorImpl<snippy::ProbableElement<_elem_type>> &>(   \
            Items),                                                            \
        true, Ctx);                                                            \
    if (!Io.outputting() && !Io.error())                                       \
      if (auto ErrMsg =                                                        \
              snippy::detail::validateProbableItems<_elem_type>(Items);        \
          !ErrMsg.empty())                                                     \
        Io.setError(ErrMsg);                                                   \
  }                                                                            \
  } // namespace yaml
#endif // LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLPROBABLEITEMS_H
