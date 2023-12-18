//===-- PluginWrapper.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "snippy/Plugins/PluginCInterface.h"
#include "snippy/Support/OpcodeGenerator.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

namespace llvm {
namespace snippy {

class OpcodeCache;

unsigned getOpcodeFromStr(const char *Str,
                          const OpcodeCacheHandle *OpcCacheHandle);
void *allocateMemory(unsigned Size);

class PluginManager final {
  class Plugin final : public OpcodeGeneratorInterface {
    class GenCommunicator final {
      const PluginFunctionsTable *DLTable;

    public:
      GenCommunicator(const PluginFunctionsTable *DLTable) : DLTable{DLTable} {}

      bool pluginHasBeenLoaded() const { return DLTable != nullptr; }

      unsigned generate(int GeneratorID) const {
        assert(pluginHasBeenLoaded());
        return DLTable->generate(GeneratorID);
      }

      int sendOpcodes(const std::vector<unsigned> &OpcodesToSend) const {
        assert(OpcodesToSend.size() != 0);
        assert(pluginHasBeenLoaded());
        Opcodes OpcStruct;
        OpcStruct.Num = OpcodesToSend.size();
        OpcStruct.Data = OpcodesToSend.data();
        return DLTable->sendOpcodes(OpcStruct);
      }
    };

    GenCommunicator Communicator;
    std::unordered_set<unsigned> AvailableOpcodes;
    int GeneratorID = -1;

  public:
    template <typename Iter>
    Plugin(const PluginFunctionsTable *DLTable, Iter First, Iter Last)
        : Communicator{DLTable} {
      if (std::distance(First, Last) == 0)
        report_fatal_error(
            "Plugin initialization failure: opcodes are not defined.\n"
            "This may happen when you can not generate any instruction in "
            "specific context.\n"
            "Try to increase requested number of instructions or add more "
            "available instructions.\n",
            false);
      std::vector<unsigned> OpcodesToSend;
      std::transform(First, Last, std::back_inserter(OpcodesToSend),
                     [](auto It) { return It.first; });
      AvailableOpcodes.clear();
      std::transform(First, Last,
                     std::inserter(AvailableOpcodes, AvailableOpcodes.begin()),
                     [](auto It) { return It.first; });
      GeneratorID = Communicator.sendOpcodes(OpcodesToSend);
    }

    unsigned generate() override {
      auto NewOpc = Communicator.generate(GeneratorID);
      if (AvailableOpcodes.count(NewOpc) == 0)
        report_fatal_error(
            "Plugin generated opcode doesn't fit in the current policy", false);
      return NewOpc;
    }

    void print(llvm::raw_ostream &OS) const override {
      OS << "PluginGenerator:\n";
      for (const auto Opc : AvailableOpcodes)
        OS << "     Opcode:" << Opc << "\n";
    }

    void dump() const override { print(dbgs()); }
  };

  const PluginFunctionsTable *DLTable = nullptr;

  void setParsingContext(const OpcodeCache &OpcCache) {
    SnippyContext ParsingContext;
    ParsingContext.OpcCacheHandleObj =
        reinterpret_cast<const OpcodeCacheHandle *>(&OpcCache);
    ParsingContext.allocateMemory = allocateMemory;
    ParsingContext.getOpcodeFromStr = getOpcodeFromStr;
    DLTable->setContext(ParsingContext);
  }

public:
  void loadPluginDL(const std::string &PluginLibName);
  bool pluginHasBeenLoaded() const { return DLTable != nullptr; }

  template <typename Iter>
  std::unique_ptr<Plugin> createPlugin(Iter First, Iter Last) const {
    return std::make_unique<Plugin>(DLTable, First, Last);
  }

  template <typename InsertIt>
  void parseOpcodes(const OpcodeCache &OpcCache, std::string FileName,
                    InsertIt HistogramIt) {
    assert(pluginHasBeenLoaded());
    setParsingContext(OpcCache);
    constexpr double OpcDefaultWeight = 1;
    Opcodes PluginOpcodes = {0};
    auto CanParse = DLTable->parseOpcodes(&PluginOpcodes, FileName.c_str());
    if (CanParse == PARSING_NOT_SUPPORTED)
      report_fatal_error("Plugin doesn't support parsing", false);

    if (PluginOpcodes.Num == 0 || !PluginOpcodes.Data)
      report_fatal_error("Invalid opcodes from plugin", false);

    for (unsigned i = 0; i < PluginOpcodes.Num; i++)
      HistogramIt = std::pair{PluginOpcodes.Data[i], OpcDefaultWeight};
  }

  void loadPluginLib(const std::string &PluginFile) {
    if (PluginFile == "None")
      return;
    loadPluginDL(PluginFile);
    assert(DLTable != nullptr);
  }
};

} // namespace snippy
} // namespace llvm
