//===-- GenResult.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_SNIPPY_GEN_RESULT_INFO_H
#define LLVM_TOOLS_SNIPPY_GEN_RESULT_INFO_H

#include <utility>

namespace llvm {
namespace snippy {

template <typename T> class GenResultT;

class GenResult {
public:
  GenResult(char &ID) : ID{&ID} {}

  template <typename T> bool isA() const {
    auto *TID = &(GenResultT<T>::getID());
    return ID == TID;
  }

  virtual ~GenResult() = default;

protected:
  char *ID;
};

template <typename T> class GenResultT final : public GenResult {
public:
  template <typename... Types>
  GenResultT(Types &&...Args)
      : GenResult(getID()), Value{std::forward<Types>(Args)...} {};
  T Value;

private:
  friend class GenResult;
  static char &getID();
};

template <typename T> char &GenResultT<T>::getID() {
  static char ID = 0;
  return ID;
}

} // namespace snippy
} // namespace llvm

#endif
