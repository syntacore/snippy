//===-- RandUtil.cpp --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Support/RandUtil.h"

#include "llvm/ADT/APInt.h"

using namespace llvm;

std::unique_ptr<snippy::RandEngine> snippy::RandEngine::pimpl = nullptr;

APInt snippy::RandEngine::genAPInt(unsigned Bits) {
  auto Result = APInt::getZero(Bits);
  auto U64Bits = std::numeric_limits<uint64_t>::digits;
  if (auto LeftOverBits = Bits % U64Bits) {
    auto Value = RandEngine::genInInterval((uint64_t(1) << LeftOverBits) - 1);
    Bits -= LeftOverBits;
    Result.insertBits(Value, Bits, LeftOverBits);
  }
  while (Bits) {
    auto Value =
        RandEngine::genInInterval(std::numeric_limits<uint64_t>::max());
    Bits -= U64Bits;
    Result.insertBits(Value, Bits, U64Bits);
  }
  return Result;
}

// To get random number [0, Last] we:
// 1) Generate Random APInt(later called Rand) with appopriate Width
// 2) Get a Rand % (Last + 1)
APInt snippy::RandEngine::genInInterval(APInt Last) {
  auto OriginalWidth = Last.getBitWidth();
  // This logic needs to prevent Last + 1 from overflow
  // For examle, when OriginalWidth=2  and Last==3, to get
  // a Last+1=4, that does not fit in 2 bits.
  // FIXME: basically, there we break the random distribution.
  bool IsMax = Last.isMaxValue();
  auto WidthForGen = IsMax ? OriginalWidth + 1 : OriginalWidth;
  auto Result = genAPInt(WidthForGen).urem(Last.zext(WidthForGen) + 1);
  if (IsMax)
    Result = Result.trunc(OriginalWidth);
  return Result;
}
