//===--- InstructionEnumerator.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_LIB_INSTRUCTION_ENUMERATOR_H
#define LLVM_TOOLS_LLVM_IE_LIB_INSTRUCTION_ENUMERATOR_H

#include "TargetEnum.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <memory>
#include <set>

namespace llvm_ie {

class InstructionEnumerator {
public:
  using PluginID = TargetEnum;
  using PluginCreateCallback =
      std::function<std::unique_ptr<InstructionEnumerator>()>;

  static std::unique_ptr<InstructionEnumerator> findPlugin(PluginID plugin_id) {
    auto it = InstructionEnumerator::s_registered_plugins.find(plugin_id);
    if (it == InstructionEnumerator::s_registered_plugins.end())
      return nullptr;
    auto callback = it->getSecond();
    return callback();
  }

  InstructionEnumerator() = default;

  virtual std::set<llvm::StringRef> enumerateInstructions() const = 0;

  virtual ~InstructionEnumerator() = default;

  static llvm::DenseMap<InstructionEnumerator::PluginID,
                        InstructionEnumerator::PluginCreateCallback>
      s_registered_plugins;
};

} // namespace llvm_ie

#endif
