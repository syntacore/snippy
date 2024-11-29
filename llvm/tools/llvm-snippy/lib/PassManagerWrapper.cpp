//===-- PassManagerWrapper.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CFGPrinter.h"
#include "snippy/CreatePasses.h"

#include "snippy/ActiveImmutablePass.h"
#include "snippy/PassManagerWrapper.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
namespace snippy {
std::unordered_map<char *, std::unique_ptr<char>>
    ActiveImmutablePassInterface::IDStash;
std::vector<std::string> ActiveImmutablePassInterface::StringStorage;

namespace {
auto lookupPassInfoAndPassArgument(AnalysisID ID) {
  auto *PI = Pass::lookupPassInfo(ID);
  std::string PA;
  if (PI && !PI->isAnalysis())
    PA = PI->getPassArgument();
  return std::make_pair(PI, std::move(PA));
}
} // namespace

void PassManagerWrapper::add(ImmutablePass *P) { PM.add(P); }

void PassManagerWrapper::add(ActiveImmutablePassInterface *P) {
  auto *AsP = P->getAsPass();
  auto *Decoy = P->createStoragePass();

  auto [PI, PA] = lookupPassInfoAndPassArgument(AsP->getPassID());
  PM.add(Decoy);

  if (PI && shouldDumpCFGBeforePass(PA))
    PM.add(createCFGPrinterPassBefore(*PI, shouldViewCFGBeforePass(PA)));

  PM.add(AsP);

  if (PI && shouldDumpCFGAfterPass(PA))
    PM.add(createCFGPrinterPassAfter(*PI, shouldViewCFGAfterPass(PA)));
}

void PassManagerWrapper::add(Pass *P) {
  assert(P && "Non-null pointer to a pass expected");

  auto [PI, PA] = lookupPassInfoAndPassArgument(P->getPassID());

  if (PI && shouldDumpCFGBeforePass(PA))
    PM.add(createCFGPrinterPassBefore(*PI, shouldViewCFGBeforePass(PA)));

  PM.add(P);

  if (PI && shouldDumpCFGAfterPass(PA))
    PM.add(createCFGPrinterPassAfter(*PI, shouldViewCFGAfterPass(PA)));
}

bool PassManagerWrapper::addAsmPrinter(LLVMTargetMachine &LLVMTM,
                                       raw_pwrite_stream &Out,
                                       raw_pwrite_stream *DwoOut,
                                       CodeGenFileType FileType,
                                       MCContext &Context) {
  return LLVMTM.addAsmPrinter(PM, Out, DwoOut, FileType, Context);
}

} // namespace snippy
} // namespace llvm
