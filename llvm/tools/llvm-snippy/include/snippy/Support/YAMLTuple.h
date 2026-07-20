//===-- YAMLTuple.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLTUPLE_H
#define LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLTUPLE_H

#include "snippy/Support/YAMLUtils.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/YAMLTraits.h"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace llvm {
namespace snippy {
// Allows to map a fixed-size YAML flow sequence like [a, b, 12]
// onto a struct. Can have members of any kind, including tuples/sequences.
// Specialize YAMLTupleTraits<T> and invoke LLVM_SNIPPY_YAML_IS_TUPLE.
template <typename T, typename Enable = void> struct YAMLTupleTraits {
  // Required: static auto members(T &Val) { return std::tie(Val.A, ...); }
  // Optional: static std::string validate(const T &);
  // Optional: static constexpr StringLiteral Name;
};

template <typename T, typename Enable = void>
struct has_YAMLTupleMembers : std::false_type {};

template <typename T>
struct has_YAMLTupleMembers<
    T, std::void_t<decltype(YAMLTupleTraits<T>::members(std::declval<T &>()))>>
    : std::true_type {};

template <typename T>
constexpr inline bool has_YAMLTupleTraits_v = has_YAMLTupleMembers<T>::value;

template <typename T, typename Enable = void>
struct has_YAMLTupleName : std::false_type {};

template <typename T>
struct has_YAMLTupleName<T, std::void_t<decltype(YAMLTupleTraits<T>::Name)>>
    : std::true_type {};

template <typename T, typename Enable = void>
struct has_YAMLTupleValidate : std::false_type {};

template <typename T>
struct has_YAMLTupleValidate<
    T, std::enable_if_t<std::is_invocable_r_v<
           std::string, decltype(YAMLTupleTraits<T>::validate), const T &>>>
    : std::true_type {};

namespace detail {

template <typename T>
using YAMLTupleMembers_t =
    std::remove_reference_t<decltype(YAMLTupleTraits<T>::members(
        std::declval<T &>()))>;

template <typename T>
constexpr inline size_t YAMLTupleSize_v =
    std::tuple_size_v<YAMLTupleMembers_t<T>>;

// A block-formatted member would break flow output
template <typename ElemT> constexpr bool isFlowSafeTupleMember() {
  using DecayedT = std::remove_cv_t<std::remove_reference_t<ElemT>>;
  constexpr bool IsBlockMap =
      yaml::has_MappingTraits<DecayedT, yaml::EmptyContext>::value &&
      !yaml::has_FlowTraits<yaml::MappingTraits<DecayedT>>::value;
  constexpr bool IsBlockSeq =
      yaml::has_SequenceTraits<DecayedT>::value &&
      !yaml::has_FlowTraits<yaml::SequenceTraits<DecayedT>>::value;
  return !IsBlockMap && !IsBlockSeq;
}

template <typename TupleT, size_t... Indices>
constexpr bool areAllTupleMembersFlowSafe(std::index_sequence<Indices...>) {
  return (isFlowSafeTupleMember<std::tuple_element_t<Indices, TupleT>>() &&
          ...);
}

// Prepends `YAMLTupleTraits<T>::Name` when it's present
template <typename T> std::string labelMsg(const Twine &Msg) {
  if constexpr (has_YAMLTupleName<T>::value)
    return (Twine(YAMLTupleTraits<T>::Name) + ": " + Msg).str();
  else
    return Msg.str();
}

template <typename T>
void diagnoseTupleSizeError(yaml::IO &Io, size_t Expected, size_t Got) {
  Io.setError(labelMsg<T>(Twine("expected ") + Twine(Expected) +
                          " element(s) in the sequence, got " + Twine(Got)));
}

template <typename ElemT>
void yamlizeTupleElement(yaml::IO &Io, ElemT &Elem, unsigned Index,
                         yaml::EmptyContext &Ctx) {
  void *SaveInfo = nullptr;
  // preflightFlowElement returns false
  // once an error has been latched.
  if (Io.preflightFlowElement(Index, SaveInfo)) {
    yamlize(Io, Elem, true, Ctx);
    Io.postflightFlowElement(SaveInfo);
  }
}

template <typename T>
void yamlizeTuple(yaml::IO &Io, T &Val, yaml::EmptyContext &Ctx) {
  static_assert(has_YAMLTupleTraits_v<T>,
                "Specialization of snippy::YAMLTupleTraits<T> with members() "
                "must be present");
  static_assert(yaml::missingTraits<T, yaml::EmptyContext>::value,
                "YAML tuple types should not define other YAML traits");
  using MembersT = YAMLTupleMembers_t<T>;
  constexpr size_t NumMembers = YAMLTupleSize_v<T>;
  static_assert(NumMembers > 0, "YAML tuples must have at least one member");
  static_assert(
      areAllTupleMembersFlowSafe<MembersT>(
          std::make_index_sequence<NumMembers>{}),
      "Tuple members must be flow-formatted - a block sequence/mapping inside "
      "a flow tuple breaks the output");

  unsigned InCount = Io.beginFlowSequence();
  bool SizeOk = Io.outputting() || InCount == NumMembers;
  if (!SizeOk && !Io.error())
    diagnoseTupleSizeError<T>(Io, NumMembers, InCount);

  if (SizeOk) {
    auto Members = YAMLTupleTraits<T>::members(Val);
    // unpack tuple members and yamlize them one by one
    std::apply(
        [&](auto &...Elems) {
          unsigned Index = 0;
          ((yamlizeTupleElement(Io, Elems, Index++, Ctx)), ...);
        },
        Members);
  }
  Io.endFlowSequence();

  if constexpr (has_YAMLTupleValidate<T>::value) {
    if (!Io.outputting() && !Io.error()) {
      if (auto ErrMsg = YAMLTupleTraits<T>::validate(std::as_const(Val));
          !ErrMsg.empty())
        Io.setError(ErrMsg);
    }
  }
}

} // namespace detail
} // namespace snippy
} // namespace llvm

#define LLVM_SNIPPY_YAML_IS_TUPLE(_type)                                       \
  namespace yaml {                                                             \
  void yamlize(yaml::IO &Io, _type &Val, bool, EmptyContext &Ctx) {            \
    llvm::snippy::detail::yamlizeTuple(Io, Val, Ctx);                          \
  }                                                                            \
  } // namespace yaml
#endif // LLVM_TOOLS_LLVM_SNIPPY_SUPPORT_YAMLTUPLE_H
