//===-- CodeAddrSampler.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLER_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLER_H

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Generator/RandomMemAccSampler.h"

namespace llvm {
namespace snippy {

/// \brief Class to randomly sample code sections addresses according to the
/// immutable CodeLayout configuration. Unlike MemoryScheme it samples addresses
/// from RX sections
class CodeAddrSampler : private RandomMemoryAccessSampler {
public:
  CodeAddrSampler(const CodeLayoutConfig &Config,
                  const SectionsDescriptions &Sections,
                  Align Alignment);

  AddressInfo randomAddress(const AddressGenInfo &Params);
  using RandomMemoryAccessSampler::print;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_CODEADDRSAMPLER_H
