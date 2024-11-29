//===-- SnippyFunctionMetadata.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once
#include "llvm/Pass.h"

#include <unordered_map>

namespace llvm {
class MachineBasicBlock;
class MachineFunction;

namespace snippy {

struct SnippyFunctionMetadata {
  const MachineBasicBlock *RegsInitBlock = nullptr;
};

class SnippyFunctionMetadataWrapper final : public ImmutablePass {
public:
  static char ID;
  SnippyFunctionMetadataWrapper() : ImmutablePass(ID) {}
  StringRef getPassName() const override {
    return "Snippy Function Metadata Wrapper Pass";
  }

  bool has(const MachineFunction &MF) { return MetadataMap.count(&MF); }
  auto &get(const MachineFunction &MF) {
    return MetadataMap.try_emplace(&MF, SnippyFunctionMetadata{}).first->second;
  }

private:
  std::unordered_map<const MachineFunction *, SnippyFunctionMetadata>
      MetadataMap;
};

} // namespace snippy
} // namespace llvm
