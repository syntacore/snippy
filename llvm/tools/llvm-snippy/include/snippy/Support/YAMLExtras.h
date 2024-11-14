//===-- YAMLExtras.h --------------------------------------------*- C++ -*-===//
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

namespace yaml {

template <typename T>
std::enable_if_t<snippy::has_ScalarTraitsWithStringError<T>::value, void>
yamlize(yaml::IO &Io, T &Val, bool, EmptyContext &Ctx) {
  if (Io.outputting()) {
    SmallString<128> Storage;
    raw_svector_ostream Buffer(Storage);
    ScalarTraits<T, void>::output(Val, Io.getContext(), Buffer);
    StringRef Str = Buffer.str();
    Io.scalarString(Str, ScalarTraits<T, void>::mustQuote(Str));
  } else {
    StringRef Str;
    Io.scalarString(Str, ScalarTraits<T, void>::mustQuote(Str));
    auto Result = ScalarTraits<T, void>::input(Str, Io.getContext(), Val);
    if (!Result.empty()) {
      Io.setError(Twine(Result));
    }
  }
}

} // namespace yaml

} // namespace llvm
