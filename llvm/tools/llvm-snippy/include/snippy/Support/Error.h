//===-- Error.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace snippy {

enum class Errc {
  // Skipping 0 value intentionally
  Unimplemented = 1,
  InvalidArgument,
  OutOfRegisters,
  InvalidConfiguration,
  LogicError,
  Failure,
  OutOfSpace,
  CorruptedElfImage,
};

std::error_code makeErrorCode(Errc Code);

// A class representing failures that happened within llvm-snippy, they are
// used to report information to the user.
class Failure : public StringError {
public:
  Failure(std::error_code EC, const Twine &S) : StringError(S, EC) {}

  Failure(std::error_code EC, const Twine &Prefix, const Twine &Desc)
      : StringError(Prefix + ": " + Desc, EC) {}
};

template <typename... Ts> Error makeFailure(std::error_code EC, Ts &&...Args) {
  return make_error<Failure>(EC, std::forward<Ts>(Args)...);
}

template <typename... Ts> Error makeFailure(Errc EC, Ts &&...Args) {
  return makeFailure(makeErrorCode(EC), std::forward<Ts>(Args)...);
}

class NoAvailableRegister : public Failure {
public:
  NoAvailableRegister(const MCRegisterClass &RegClass,
                      const MCRegisterInfo &RegInfo, const Twine &Msg)
      : Failure(makeErrorCode(Errc::OutOfRegisters),
                Twine("No available register ") +
                    RegInfo.getRegClassName(&RegClass) + ": " + Msg) {}
};

class MemoryAccessSampleError : public StringError {
public:
  MemoryAccessSampleError(const Twine &S)
      : StringError(S, inconvertibleErrorCode()) {}
};
} // namespace snippy
} // namespace llvm
