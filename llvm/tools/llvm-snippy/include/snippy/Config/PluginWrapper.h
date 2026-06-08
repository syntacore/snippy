//===-- PluginWrapper.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_TOOLS_LLVM_SNIPPY_CONFIG_PLUGINWRAPPER_H
#define LLVM_TOOLS_LLVM_SNIPPY_CONFIG_PLUGINWRAPPER_H

#include "snippy/Plugins/PluginCInterface.h"
#include "snippy/Support/DiagnosticInfo.h"
#include "snippy/Support/OpcodeGenerator.h"

#include "llvm/ADT/SmallVector.h"

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
    OpcodeHistogram OpcodeHist;
    std::unordered_set<unsigned> AvailableOpcodes;
    int GeneratorID = -1;

  public:
    Plugin(const PluginFunctionsTable *DLTable, const OpcodeHistogram &OpcHist)
        : Communicator{DLTable}, OpcodeHist(OpcHist) {
      if (OpcodeHist.empty())
        snippy::fatal(
            "Plugin initialization failure",
            "opcodes are not defined."
            "This may happen when you can not generate any instruction in "
            "specific context.\n"
            "Try to increase requested number of instructions or add more "
            "available instructions.");
      std::vector<unsigned> OpcodesToSend;
      llvm::transform(llvm::make_first_range(OpcHist.topOpcodes()),
                      std::back_inserter(OpcodesToSend),
                      [](auto &&Opc) { return Opc; });
      AvailableOpcodes.clear();
      llvm::transform(llvm::make_first_range(OpcHist.topOpcodes()),
                      std::inserter(AvailableOpcodes, AvailableOpcodes.begin()),
                      [](auto &&Opc) { return Opc; });
      GeneratorID = Communicator.sendOpcodes(OpcodesToSend);
    }

    void generate(SmallVectorImpl<unsigned> &Opcodes) override {
      auto Opcode = Communicator.generate(GeneratorID);
      if (AvailableOpcodes.count(Opcode) == 0)
        snippy::fatal("Plugin opcode",
                      "generated opcode doesn't fit in the current policy");
      Opcodes.push_back(Opcode);
    }

    std::unique_ptr<OpcodeGeneratorInterface> copy() const override {
      return std::make_unique<Plugin>(*this);
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

  std::unique_ptr<Plugin> createPlugin(const OpcodeHistogram &OpcHist) const {
    return std::make_unique<Plugin>(DLTable, OpcHist);
  }

  void parseOpcodes(const OpcodeCache &OpcCache, std::string FileName,
                    OpcodeHistogram &OpcHist) {
    assert(pluginHasBeenLoaded());
    setParsingContext(OpcCache);
    constexpr double OpcDefaultWeight = 1;
    Opcodes PluginOpcodes = {0, nullptr};
    auto CanParse = DLTable->parseOpcodes(&PluginOpcodes, FileName.c_str());
    if (CanParse == PARSING_NOT_SUPPORTED)
      snippy::fatal("Plugin doesn't support parsing.");

    if (PluginOpcodes.Num == 0 || !PluginOpcodes.Data)
      snippy::fatal("Invalid opcodes from plugin.");

    SmallVector<std::pair<unsigned, double>> OpcWeightRange;
    for (unsigned i = 0; i < PluginOpcodes.Num; i++)
      OpcWeightRange.emplace_back(PluginOpcodes.Data[i], OpcDefaultWeight);
    OpcHist.insertTopOpcodes(std::move(OpcWeightRange));
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
#endif // LLVM_TOOLS_LLVM_SNIPPY_CONFIG_PLUGINWRAPPER_H
