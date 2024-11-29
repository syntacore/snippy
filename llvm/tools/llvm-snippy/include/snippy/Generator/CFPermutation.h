//===-- CFPermutation.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <set>
#include <unordered_map>

namespace llvm {
class MachineBasicBlock;
class MachineFunction;
class raw_ostream;
namespace snippy {
struct Branchegram;
class GeneratorContext;
class FunctionGenerator;
struct SimulatorContext;

class ConsecutiveLoopInfo {
public:
  ConsecutiveLoopInfo() = default;
  bool isFirstConsecutiveLoopHeader(unsigned BB) const {
    return ConsecutiveLoopsHeaders.count(BB);
  }

  bool isNonFirstConsecutiveLoopHeader(unsigned BB) const {
    return any_of(make_second_range(ConsecutiveLoopsHeaders),
                  [BB](const auto &ConsLoops) { return ConsLoops.count(BB); });
  }

  void registerConsecutiveLoopsHeader(unsigned ConsecutiveLoopHeader,
                                      unsigned FirstLoopHeader) {
    ConsecutiveLoopsHeaders[FirstLoopHeader].insert(ConsecutiveLoopHeader);
  }

  const auto &getConsecutiveLoops(unsigned FirstConsecutiveLoop) const {
    assert(isFirstConsecutiveLoopHeader(FirstConsecutiveLoop));
    return ConsecutiveLoopsHeaders.at(FirstConsecutiveLoop);
  }

  const auto &getConsecutiveLoops() const { return ConsecutiveLoopsHeaders; }

private:
  //       +---v---+
  //       |       |
  //       +---+---+
  //           | ______    ------------------------------
  //           |/      \
  //       +---v---+   |
  //       |       |   |      First consecutive loop
  //       +---+---+   |
  //           |\______/
  //           | ______    ------------------------------
  //           |/      \
  //       +---v---+   |
  //       |       |   |
  //       +---+---+   |
  //           |\______/
  //           | ______
  //           |/      \
  //       +---v---+   |
  //       |       |   |      Other consecutive loops
  //       +---+---+   |
  //           |\______/
  //           | ______
  //           |/      \
  //       +---v---+   |
  //       |       |   |
  //       +---+---+   |
  //           |\______/
  //           |           ------------------------------
  //           |
  //       +---v---+
  //       |       |
  //       +---+---+
  //
  // First loop header <-> Consecutive loops headers (basic blocks numbers)
  std::unordered_map<unsigned, std::set<unsigned>> ConsecutiveLoopsHeaders;
};

class CFPermutationContext {
public:
  struct BlockInfo final {
    unsigned Successor;
    unsigned IfDepth = 0;
    unsigned LoopDepth = 0;
    using SetT = std::set<unsigned>;
    SetT Available;

    BlockInfo(unsigned Succ) : Successor(Succ), Available() {}

    BlockInfo(unsigned Succ, const SetT &Available)
        : Successor(Succ), Available(Available) {}
  };

  using BlocksInfoT = SmallVector<BlockInfo>;
  using BlocksInfoIter = BlocksInfoT::iterator;

protected:
  /// BlocksInfo is a table with permutation context infromation.
  /// It has next structure and initial value (for N blocks or N - 1 branches):
  ///
  /// | BB Idx | Successor | IfDepth | LoopDepth | AvailableSet  |
  /// +--------+-----------+---------+-----------+---------------+
  /// |      0 |         1 |       0 |         0 | { 0, ..., N } |
  /// |      1 |         2 |       0 |         0 | { 0, ..., N } |
  /// |    ... |       ... |       0 |         0 | { 0, ..., N } |
  /// |      k |     k + 1 |       0 |         0 | { 0, ..., N } |
  /// |    ... |       ... |       0 |         0 | { 0, ..., N } |
  /// |  N - 1 |         N |       0 |         0 | { 0, ..., N } |
  BlocksInfoT BlocksInfo;
  std::reference_wrapper<MachineFunction> CurrMF;
  std::reference_wrapper<GeneratorContext> GC;
  std::reference_wrapper<FunctionGenerator> FG;
  std::reference_wrapper<ConsecutiveLoopInfo> CLI;
  std::reference_wrapper<const Branchegram> BranchSettings;
  std::reference_wrapper<const SimulatorContext> SimCtx;
  unsigned PermutationCounter = 0;

public:
  CFPermutationContext(MachineFunction &MF, GeneratorContext &GC,
                       FunctionGenerator &FG, ConsecutiveLoopInfo &CLI,
                       const SimulatorContext &SimCtx);
  virtual ~CFPermutationContext() = default;

  virtual void print(raw_ostream &OS) const;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  virtual void dump() const;
#endif

  virtual bool makePermutationAndUpdateBranches();

  static unsigned calculateMaxDistance(unsigned BBNum, unsigned Size,
                                       unsigned RequestedInstrsNum,
                                       unsigned MaxBranchDstMod,
                                       GeneratorContext &GC);

  static SmallVector<unsigned> generatePermutationOrder(unsigned Size);

protected:
  void initBlocksInfo(unsigned Size);
  MachineBasicBlock *getBlockNumbered(unsigned N);
  const MachineBasicBlock *getBlockNumbered(unsigned N) const;
  void makePermutation(ArrayRef<unsigned> PermutationOrder);
  void permuteBlock(unsigned BB);
  void updateBlocksInfo(unsigned From, unsigned To);
  void dumpPermutedBranchIfNeeded(unsigned BB, bool IsLoop,
                                  const BlockInfo &BI) const;
  bool updateBranches();
  void clear();

  BlocksInfoIter findMaxIfDepthReached(BlocksInfoIter Beg, BlocksInfoIter End);
  BlocksInfoIter findMaxLoopDepthReached(BlocksInfoIter Beg,
                                         BlocksInfoIter End);
  size_t getCFInstrNumFor(const MachineFunction &MF) const;

private:
  void initOneBlockInfo(unsigned BB, unsigned NBlocks,
                        size_t RequestedInstrsNum);
};

} // namespace snippy
} // namespace llvm
