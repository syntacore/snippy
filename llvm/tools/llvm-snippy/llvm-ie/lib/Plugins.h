//===--- Plugins.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_LIB_PLUGINS_H
#define LLVM_TOOLS_LLVM_IE_LIB_PLUGINS_H

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/WithColor.h"

#define LLVM_IE_TARGET_DECLARE(TargetName)                                     \
  namespace llvm_ie {                                                          \
  extern void llvm_ie_initialize_##TargetName();                               \
  extern void llvm_ie_terminate_##TargetName();                                \
  }

#define LLVM_IE_TARGET_DEFINE(TargetName)                                      \
  namespace llvm_ie {                                                          \
  void llvm_ie_initialize_##TargetName() { TargetName::initialize(); }         \
  void llvm_ie_terminate_##TargetName() { TargetName::terminate(); }           \
  }

#define LLVM_IE_TARGET_INITIALIZE(TargetName) llvm_ie_initialize_##TargetName();
#define LLVM_IE_TARGET_TERMINATE(TargetName) llvm_ie_terminate_##TargetName();

namespace llvm_ie {
namespace plugins {

template <typename Plugin>
void registerPlugin(typename Plugin::PluginID plugin_id,
                    typename Plugin::PluginCreateCallback callback) {
  if (!callback) {
    llvm::WithColor::error()
        << "Plugin with id " << plugin_id << " has invalid create callback!\n";
    return;
  }

  if (auto it = Plugin::s_registered_plugins.find(plugin_id);
      it != Plugin::s_registered_plugins.end()) {
    llvm::WithColor::error()
        << "Plugin with id " << plugin_id << " already exists!\n";
    return;
  }

  auto [it, check] = Plugin::s_registered_plugins.insert({plugin_id, callback});
  if (!check)
    llvm::WithColor::error()
        << "Can't register plugin with id " << plugin_id << "!\n";
}

template <typename Plugin>
void unregisterPlugin(typename Plugin::PluginID plugin_id) {
  if (!Plugin::s_registered_plugins.erase(plugin_id))
    llvm::WithColor::error()
        << "Can't find plugin with id " << plugin_id << "!\n";
}

} // namespace plugins
} // namespace llvm_ie

#endif
