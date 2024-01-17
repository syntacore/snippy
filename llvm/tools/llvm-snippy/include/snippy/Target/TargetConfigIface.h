//===-- TargetConfigIface.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

namespace llvm {

namespace yaml {
class IO;
}

namespace snippy {

class TargetConfigInterface {
public:
  virtual ~TargetConfigInterface(){};

  template <typename ImplT> ImplT &getImpl() {
    return static_cast<ImplT &>(*this);
  }

  template <typename ImplT> const ImplT &getImpl() const {
    return static_cast<const ImplT &>(*this);
  }

  virtual bool hasConfig() const = 0;
  virtual void mapConfig(yaml::IO &IO) = 0;
};

} // namespace snippy
} // namespace llvm
