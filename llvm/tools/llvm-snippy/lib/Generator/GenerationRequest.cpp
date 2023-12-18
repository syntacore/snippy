//===-- GenerationRequest.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/GenerationRequest.h"

namespace llvm {
namespace snippy {

void GenerationStatistics::print(raw_ostream &OS) const {
  OS << "GenStats={ "
     << "NumOfInstrs: " << NumOfInstrs << ", GeneratedSize: " << GeneratedSize
     << " }";
}

raw_ostream &operator<<(raw_ostream &OS, GenerationMode GM) {
  switch (GM) {
  case GenerationMode::NumInstrs:
    OS << "NumInstrs";
    break;
  case GenerationMode::Size:
    OS << "Size";
    break;
  case GenerationMode::Mixed:
    OS << "Mixed";
    break;
  default:
    OS << "Unknown";
  }
  return OS;
}

MBBGenReqIterator::MBBGenReqIterator(IMBBGenReq &Req)
    : Req(Req), Elem(Req.nextSubRequest()) {}

MBBGenReqIterator::MBBGenReqIterator(IMBBGenReq &Req, std::nullptr_t)
    : Req(Req), Elem(nullptr) {}

MBBGenReqIterator &MBBGenReqIterator::operator++() {
  Elem = Req.nextSubRequest();
  return *this;
}

} // namespace snippy
} // namespace llvm
