//===-- YAMLHistogram.h------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Support/YAMLUtils.h"

#include "llvm/Support/YAMLTraits.h"

namespace llvm {
namespace snippy {
// This traits class is used to parse a generic weighted histogram from YAML
// using LLVM YAML I/O library. Such a histogram has the following format:
//
// histogram-name:
//   - [key1, weight1]
//   - [key2, weight2]
//   ....
//
// Parsed histogram will be of type MapType

template <typename T, typename Enable = void> struct YAMLHistogramTraits {
  // The user has to specify the following functions/aliases:
  // using MapType = ....;

  // static T denormalizeEntry(yaml::IO &Io, StringRef Key, double Weight);
  // static void normalizeEntry(yaml::IO &Io, const T &,
  //                            SmallVectorImpl<SValue>
  //                            &);

  // static MapType denormalizeMap(yaml::IO &Io, ArrayRef<T>);
  // static void normalizeMap(yaml::IO &Io, const MapType &,
  //                          std::vector<T> &);
  // static std::string validate(ArrayRef<T>);
  //
  // User can opt in to automatic double parsing. Then denormalizeEntry will
  // have the following signature.
  // static constexpr bool ParseArbitraryValue = true;
  // static T denormalizeEntry(yaml::IO &Io, StringRef Key, StringRef Weight);
};

template <typename T>
using YAMLHistogramMapType_t = typename YAMLHistogramTraits<T>::MapType;

template <typename T, typename Enable = void>
struct has_YAMLHistogramMapType : std::false_type {};

template <typename T>
struct has_YAMLHistogramMapType<T, std::void_t<YAMLHistogramMapType_t<T>>>
    : std::true_type {};

template <typename T, typename Enable = void>
struct YAMLHistogramArbitraryValue {
  static constexpr bool Value = false;
};

template <typename T>
struct YAMLHistogramArbitraryValue<
    T, std::void_t<decltype(YAMLHistogramTraits<T>::ParseArbitraryValue)>> {
  static constexpr bool Value = YAMLHistogramTraits<T>::ParseArbitraryValue;
};

template <typename T>
constexpr inline auto YAMLHistogramArbitraryValue_v =
    YAMLHistogramArbitraryValue<T>::Value;

template <typename T, typename Enable = void>
struct has_YAMLHistogramDenormalizeEntry : std::false_type {};

template <typename T>
struct has_YAMLHistogramDenormalizeEntry<
    T, std::enable_if_t<std::is_invocable_r_v<
           T, decltype(YAMLHistogramTraits<T>::denormalizeEntry), yaml::IO &,
           StringRef,
           std::conditional_t<YAMLHistogramArbitraryValue_v<T>, StringRef,
                              double>>>> : std::true_type {};

template <typename T, typename Enable = void>
struct has_YAMLHistogramNormalizeEntry : std::false_type {};

template <typename T>
struct has_YAMLHistogramNormalizeEntry<
    T, std::enable_if_t<std::is_invocable_r_v<
           void, decltype(YAMLHistogramTraits<T>::normalizeEntry), yaml::IO &,
           const T &, SmallVectorImpl<SValue> &>>> : std::true_type {};

template <typename T, typename Enable = void>
struct has_YAMLHistogramDenormalizeMap : std::false_type {};

template <typename T>
struct has_YAMLHistogramDenormalizeMap<
    T, std::enable_if_t<std::is_invocable_r_v<
           YAMLHistogramMapType_t<T>,
           decltype(YAMLHistogramTraits<T>::denormalizeMap), yaml::IO &,
           ArrayRef<T>>>> : std::true_type {};

template <typename T, typename Enable = void>
struct has_YAMLHistogramNormalizeMap : std::false_type {};

template <typename T>
struct has_YAMLHistogramNormalizeMap<
    T, std::enable_if_t<std::is_invocable_r_v<
           void, decltype(YAMLHistogramTraits<T>::normalizeMap), yaml::IO &,
           YAMLHistogramMapType_t<T>, std::vector<T> &>>> : std::true_type {};

template <typename T, typename Enable = void>
struct has_YAMLHistogramTraits : std::false_type {};

template <typename T>
struct has_YAMLHistogramTraits<
    T,
    std::enable_if_t<std::conjunction_v<
        has_YAMLHistogramMapType<T>, has_YAMLHistogramDenormalizeEntry<T>,
        has_YAMLHistogramNormalizeEntry<T>, has_YAMLHistogramDenormalizeMap<T>,
        has_YAMLHistogramNormalizeMap<T>>>> : std::true_type {};

template <typename T>
constexpr inline bool has_YAMLHistogramTraits_v =
    has_YAMLHistogramTraits<T>::value;

template <typename T> struct YAMLHistogramNormEntry {
  using DenormEntry = T;

  YAMLHistogramNormEntry(yaml::IO &) {}
  YAMLHistogramNormEntry(yaml::IO &Io, const DenormEntry &E) {
    YAMLHistogramTraits<T>::normalizeEntry(Io, E, Elements);
  }

  static constexpr const char *getValueDiagnoseName() {
    if constexpr (YAMLHistogramArbitraryValue_v<T>) {
      return "value";
    } else {
      return "weight";
    }
  }

  bool diagnoseObviousErrors(yaml::IO &Io) const {
    static constexpr auto DiagValueName = getValueDiagnoseName();

    if (Elements.size() < 2) {
      Io.setError("Incorrect histogram: Key and " + Twine(DiagValueName) +
                  " must be specified and separated "
                  "with a comma");
      return true;
    }

    if (Elements.size() > 2) {
      Io.setError("Incorrect histogram: Only two parameters (key and " +
                  Twine(DiagValueName) + ") are allowed in a histogram");
      return true;
    }

    return false;
  }

  DenormEntry denormalize(yaml::IO &Io) const {
    DenormEntry E;

    if (diagnoseObviousErrors(Io))
      return E;

    StringRef KeyStr = Elements[0].value, WeightStr = Elements[1].value;

    if constexpr (YAMLHistogramArbitraryValue_v<DenormEntry>) {
      return YAMLHistogramTraits<T>::denormalizeEntry(Io, KeyStr, WeightStr);
    } else {
      double Weight = 0.0;
      if (WeightStr.getAsDouble(Weight, true)) {
        Io.setError(
            "Incorrect histogram: Weight must be a floating point value");
        return E;
      }
      return YAMLHistogramTraits<T>::denormalizeEntry(Io, KeyStr, Weight);
    }
  }

  SmallVector<SValue, 2> Elements;
};

template <typename T> struct YAMLHistogramNormKeyWeightMap {
  using MapType = YAMLHistogramMapType_t<T>;

  YAMLHistogramNormKeyWeightMap(yaml::IO &) {}
  YAMLHistogramNormKeyWeightMap(yaml::IO &Io, const MapType &M) {
    YAMLHistogramTraits<T>::normalizeMap(Io, M, Entries);
  }

  MapType denormalize(yaml::IO &Io) const {
    if (Invalid)
      return MapType{};
    return YAMLHistogramTraits<T>::denormalizeMap(Io, Entries);
  }

  std::string validate() {
    auto ErrMsg = YAMLHistogramTraits<T>::validate(Entries);
    Invalid = !ErrMsg.empty();
    return ErrMsg;
  }

  std::vector<T> Entries;
  bool Invalid;
};

template <typename T, typename = void> struct YAMLHistogramIO {};

template <typename T>
struct YAMLHistogramIO<T, std::enable_if_t<has_YAMLHistogramTraits_v<T>>> {
  using MapType = YAMLHistogramMapType_t<T>;
  YAMLHistogramIO(MapType &M) : UnderlyingMap(M) {}
  MapType &UnderlyingMap;
};

} // namespace snippy

namespace yaml {
template <typename T>
struct SequenceElementTraits<
    T, std::enable_if_t<snippy::has_YAMLHistogramTraits_v<T>>> {
  static constexpr bool flow = false;
};

template <typename T>
void yamlize(yaml::IO &Io, snippy::YAMLHistogramIO<T> &Hist, bool,
             EmptyContext &Ctx) {
  MappingNormalization<snippy::YAMLHistogramNormKeyWeightMap<T>,
                       snippy::YAMLHistogramMapType_t<T>>
      Norm(Io, Hist.UnderlyingMap);
  yamlize(Io, Norm->Entries, true /* not used */, Ctx);
  if (auto ErrorMsg = Norm->validate(); !ErrorMsg.empty())
    Io.setError(ErrorMsg);
}

} // namespace yaml
} // namespace llvm

#define LLVM_SNIPPY_YAML_IS_HISTOGRAM_DENORM_ENTRY(_type)                      \
  LLVM_SNIPPY_YAML_DECLARE_IS_HISTOGRAM_DENORM_ENTRY(_type) {                  \
    MappingNormalization<llvm::snippy::YAMLHistogramNormEntry<_type>, _type>   \
        Norm(Io, E);                                                           \
    yamlize(Io, Norm->Elements, true /* not used */, Ctx);                     \
  }
