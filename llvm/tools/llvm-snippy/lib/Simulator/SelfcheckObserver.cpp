//===-- SelfcheckObserver.cpp -------------------------------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Simulator/SelfcheckObserver.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/YAMLTraits.h"

#define DEBUG_TYPE "snippy-selfcheck-observer"

template <> struct llvm::yaml::MappingTraits<llvm::snippy::AnnotationPair> {
  static void mapping(IO &Io, llvm::snippy::AnnotationPair &P) {
    Io.mapRequired("address", P.Address);
    Io.mapRequired("pc", P.PC);
  }
};

template <> struct llvm::yaml::MappingTraits<llvm::snippy::SelfcheckObserver> {
  static void mapping(yaml::IO &IO, llvm::snippy::SelfcheckObserver &Observer) {
    IO.mapRequired("selfcheck-annotation", Observer.getSelfcheckAnnotation());
  }
};

LLVM_YAML_IS_SEQUENCE_VECTOR(llvm::snippy::AnnotationPair)

namespace llvm {
namespace snippy {

AnnotationPair::AnnotationPair(MemoryAddressType Addr, ProgramCounterType PCVal)
    : Address(("0x" + Twine(utohexstr(Addr))).str()),
      PC(("0x" + Twine(utohexstr(PCVal))).str()) {}

void SelfcheckObserver::memUpdateNotification(MemoryAddressType Addr,
                                              const char *Data, size_t Size) {
  auto AddrToPc = SelfcheckMap.find(Addr);
  if (AddrToPc == SelfcheckMap.end())
    return;

  assert(PCs.size() > AddrToPc->second);
  SelfcheckAnnotation.emplace_back(Addr,
                                   *std::next(PCs.rbegin(), AddrToPc->second));
}

void SelfcheckObserver::dumpAsYaml(StringRef Path) {
  std::error_code EC;
  raw_fd_ostream File(Path, EC);
  if (EC)
    report_fatal_error("Selfcheck annotation dump error: " + EC.message() +
                           ": " + Path,
                       false);

  yaml::Output Yout(File);
  Yout << *this;
}

} // namespace snippy
} // namespace llvm
