//===-- IndJumpInfo.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INDJUMPINFO_H
#define LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INDJUMPINFO_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

namespace llvm {
namespace snippy {

struct IndJumpInfo final {
  MachineBasicBlock *MBB;
  std::vector<MachineInstr *> Support;
};

class IndJumpInfoMap final
    : private DenseMap<const MachineInstr *, IndJumpInfo> {
public:
  void addJump(const MachineInstr &Jump, IndJumpInfo Dst) {
    [[maybe_unused]] auto [It, WasInserted] =
        DenseMap::try_emplace(&Jump, std::move(Dst));
    assert(WasInserted && "Attempt to specify destination for jump twice");
  }

  void remove(const MachineInstr &Jump) {
    [[maybe_unused]] auto Removed = DenseMap::erase(&Jump);
    assert(Removed);
  }

  auto &getInfo(const MachineInstr &Jump) const & {
    auto Found = DenseMap::find(&Jump);
    assert(Found != end() && "Attempt to get Destination for unknown jump");
    return Found->second;
  }

  bool contains(const MachineInstr &Jump) const {
    return DenseMap::count(&Jump);
  }

  using DenseMap::empty;
  using DenseMap::size;
};

} // namespace snippy
} // namespace llvm
#endif // LLVM_TOOLS_LLVM_SNIPPY_GENERATOR_INDJUMPINFO_H
