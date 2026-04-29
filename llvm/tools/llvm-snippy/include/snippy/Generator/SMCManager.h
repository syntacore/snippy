//===-- SMCManager.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <string>
#include <vector>

namespace llvm {

class GlobalVariable;
class MachineBasicBlock;

namespace snippy {

class Linker;
class GeneratorContext;
class Interpreter;
class LLVMState;
class SnippyTarget;

namespace planning {
class InstructionGenerationContext;
} // namespace planning

class SMCManagerT final {
  using SMCPair = std::pair<std::string, const MachineBasicBlock *>;

  std::vector<SMCPair> SMCBlockPairs;
  DenseMap<const MachineBasicBlock *, GlobalVariable *> SMCSrcMap;
  // TODO: use Register instead of unsigned or custom class
  std::vector<unsigned> SMCRegList;
  // smc target function name

public:
  static constexpr StringRef SMCTgtFuncName = "__snippy_smc_target";
  static constexpr StringRef SMCSrcFuncName = "__snippy_smc_source";
  static constexpr StringRef SMCSrcBlockPrefix = "smc_src_";
  static constexpr StringRef SMCTgtBlockSizePrefix = "size_";
  static constexpr StringRef SMCCopyFuncName = "__snippy_smc_memcpy";

  SMCManagerT() = default;

  // TODO: Split responsibilities
  std::vector<unsigned>
  getOrCreateSMCRegList(planning::InstructionGenerationContext &IGC);

  size_t getSMCSrcBlocksNum() const { return SMCBlockPairs.size(); }

  template <typename... PairArgs> void emplacePair(PairArgs &&...Args) {
    SMCBlockPairs.emplace_back(std::forward<PairArgs>(Args)...);
  }

  template <typename... PairArgs> void addToSMCSrcMap(PairArgs &&...Args) {
    auto &&[_, Res] = SMCSrcMap.try_emplace(std::forward<PairArgs>(Args)...);
    if (!Res)
      report_fatal_error("Attempt to add another one GV for MBB", false);
  }

  GlobalVariable *getGVFromSMCSrcMap(const MachineBasicBlock *MBB) {
    assert(SMCSrcMap.contains(MBB));
    return SMCSrcMap[MBB];
  }

  const std::vector<SMCPair> &getSMCBlockPairs() const { return SMCBlockPairs; }

  std::vector<unsigned> getSMCRegList() const { return SMCRegList; }

  auto getTgtBlocksFromBlockPairs() const {
    return map_range(SMCBlockPairs,
                     [](const auto &Pair) { return Pair.second; });
  }

  auto getSrcNamesFromSMCBlockPairs() const {
    return map_range(SMCBlockPairs,
                     [](const auto &Pair) { return Pair.first; });
  }
};
} // namespace snippy
} // namespace llvm
