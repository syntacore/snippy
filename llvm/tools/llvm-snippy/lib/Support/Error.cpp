//===-- Error.cpp -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/Error.h"

namespace llvm {
namespace snippy {

class SnippyErrorCategory : public std::error_category {
  const char *name() const noexcept override { return "snippy"; }
  std::string message(int ErrorValue) const override;
};

std::string SnippyErrorCategory::message(int ErrorValue) const {
  switch (static_cast<Errc>(ErrorValue)) {
  case Errc::Unimplemented:
    return "Unimplemented";
  case Errc::InvalidArgument:
    return "Invalid argument";
  case Errc::OutOfRegisters:
    return "No available registers";
  case Errc::InvalidConfiguration:
    return "Invalid configuration";
  case Errc::LogicError:
    return "Logic error";
  case Errc::Failure:
    return "Failure";
  case Errc::OutOfSpace:
    return "Out of available space";
  case Errc::CorruptedElfImage:
    return "Corrupted ELF image";
  }

  return "Unrecognized error code";
}

std::error_code makeErrorCode(Errc Code) {
  static const SnippyErrorCategory TheSnippyErrorCategory;
  return std::error_code(static_cast<int>(Code), TheSnippyErrorCategory);
}

} // namespace snippy
} // namespace llvm
