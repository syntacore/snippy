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

// A class representing failures that happened within llvm-snippy, they are
// used to report informations to the user.
class Failure : public StringError {
public:
  Failure(const Twine &S) : StringError(S, inconvertibleErrorCode()) {}

  Failure(const Twine &Prefix, const Twine &Desc)
      : StringError(Prefix + ": " + Desc, inconvertibleErrorCode()) {}
};

class NoAvailableRegister : public Failure {
public:
  NoAvailableRegister(const MCRegisterClass &RegClass,
                      const MCRegisterInfo &RegInfo, const Twine &Msg)
      : Failure(Twine("No available register ") +
                RegInfo.getRegClassName(&RegClass) + ": " + Msg) {}
};

} // namespace snippy
} // namespace llvm
