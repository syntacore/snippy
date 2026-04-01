//===-- CodeAddrSampler.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "snippy/Generator/CodeAddrSampler.h"
#include "snippy/Support/YAMLUtils.h"
#include "llvm/Support/YAMLTraits.h"

namespace llvm {
namespace snippy {

AddressInfo CodeAddrSampler::randomAddress(const AddressGenInfo &Params) {
  auto Access =
      sample(AddressGenInfo::singleAccess(Params.AccessSize, Params.Alignment,
                                          Params.AllowMisalign, /*Burst=*/false)

      );
  if (!Access)
    snippy::fatal("Code layout schemes cannot fit Basic Block",
                  Twine(toString(Access.takeError()))
                      .concat(" ")
                      .concat("Try larger code layout address ranges"));
  auto &AI = *Access;
  AI.MaxOffset = AI.MinOffset = 0;
  reserve(MemRange(AI));
  return AI;
}

template <typename SectIt>
MemoryBank createSectionsMB(SectIt Start, SectIt Finish) {
  MemoryBank SecMB;
  for (auto &S : llvm::make_range(Start, Finish))
    SecMB.addRange(MemRange{S.VMA, S.VMA + S.Size});
  return SecMB;
}

CodeAddrSampler::CodeAddrSampler(
    const CodeLayoutConfig &Config, const SectionsDescriptions &Sections,
    Align Alignment)
    : RandomMemoryAccessSampler([&]() {
        auto RXSections = llvm::make_filter_range(
            Sections, [](auto &S) { return S.M.R() && S.M.X(); });
        auto SecMB = createSectionsMB(RXSections.begin(), RXSections.end());
        auto IsContainedInSections = [&SecMB](auto &Acc) {
          auto AddrMB = Acc.getPossibleAddresses();
          return AddrMB.containedIn(SecMB);
        };
        auto ContainedAccesses =
            llvm::make_filter_range(Config.Ranges, IsContainedInSections);
        auto NotContainedAccesses = llvm::make_filter_range(
            Config.Ranges, std::not_fn(IsContainedInSections));

        for (auto &R : NotContainedAccesses) {
          std::string SchemeDump;
          raw_string_ostream SS{SchemeDump};
          MemoryScheme MAcc;
          MAcc.BaseAccesses.emplace_back(R.copy());
          outputYAMLToStream(MAcc, SS);
          LLVMContext Ctx;
          snippy::warn(
              WarningName::MemoryAccess, Ctx, "Possibly wrong code layout",
              "Following scheme permits code generation outside of all "
              "RX sections (It will be ignored)\n" +
                  Twine(SchemeDump));
        }

        return RandomMemoryAccessSampler(RXSections.begin(), RXSections.end(),
                                         ContainedAccesses.begin(),
                                         ContainedAccesses.end(),
                                         Alignment);
      }()) {
}

} // namespace snippy
} // namespace llvm
