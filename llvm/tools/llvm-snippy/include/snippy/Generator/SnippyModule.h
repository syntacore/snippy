//===-- SnippyModule.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/LLVMState.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/RegisterGenerator.h"
#include "snippy/Generator/RegisterPool.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/Debug.h"

namespace llvm {
class TargetSubtargetInfo;
} // namespace llvm

namespace llvm {
namespace snippy {

struct GeneratorResult {
  std::string SnippetImage;
  std::string LinkerScript;
};

class RegisterGenerator;
class GlobalsPool;
class OpcodeCache;
class MemoryManager;
struct SnippyProgramSettings;
class RootRegPoolWrapper;
class SnippyModule final {
public:
  SnippyModule(LLVMState &State, StringRef Name);

  const auto &getModule() const { return M; }

  auto &getModule() { return M; }

  auto &getMMI() const { return MMI; }

  const auto &getLLVMState() const { return State; }

  using PassInserter = std::function<void(PassManagerWrapper &)>;

  void generateObject(const PassInserter &Inserter);

  bool haveGeneratedObject() const { return !GeneratedObject.empty(); }

  const auto &getGeneratedObject() const { return GeneratedObject; }

private:
  LLVMState &State;
  Module M;
  std::unique_ptr<MCContext> Context;
  std::unique_ptr<MachineModuleInfoWrapperPass> MMIWP;
  MachineModuleInfo &MMI;
  std::unique_ptr<PassManagerWrapper> PPM;
  SmallString<32> GeneratedObject;
};

class SnippyProgramContext final {
public:
  SnippyProgramContext(LLVMState &State, RegisterGenerator &RegGen,
                       RegPool &Pool, const OpcodeCache &OpCc,
                       const SnippyProgramSettings &Settings);

  SnippyProgramContext(SnippyProgramContext &&) = default;
  SnippyProgramContext &operator=(SnippyProgramContext &&) = default;

  ~SnippyProgramContext();

  auto &getLLVMState() const { return *State; }

  StringRef getOutputSectionName(const Function &F) const {
    return F.hasSection() ? F.getSection() : ".text";
  }

  auto getOutputSectionFor(const Function &F) const {
    auto SectionName = getOutputSectionName(F);
    assert(PLinker->hasOutputSectionFor(SectionName));
    return PLinker->getOutputSectionFor(SectionName).Desc;
  }

  auto getOutputSectionFor(const MachineFunction &MF) const {
    auto &F = MF.getFunction();
    return getOutputSectionFor(F);
  }

  bool isManglingEnabled() const { return MangleExportedNames; }

  auto hasSelfcheckSections() const { return SelfcheckSection.has_value(); }
  auto &getSelfcheckSection() const { return *SelfcheckSection; }

  // We return by value here to enforce copy
  RegPoolWrapper getRegisterPool() {
    assert(State);
    return {State->getSnippyTarget(), State->getRegInfo(), RegPoolsStorage};
  }

  GeneratorResult generateELF(ArrayRef<const SnippyModule *> Modules) const;
  std::string generateLinkedImage(ArrayRef<const SnippyModule *> Modules) const;

  Linker &getLinker() const { return *PLinker; }
  RegisterGenerator &getRegGen() const { return *RegGen; }
  const auto &getOpcodeCache() const { return *OpCC; }

  static constexpr unsigned getSCStride() { return SCStride; }
  static constexpr unsigned getPageSize() { return kPageSize; }

  bool hasUtilitySection() const { return UtilitySection.has_value(); }

  auto &getUtilitySection() const {
    assert(hasUtilitySection());
    return *UtilitySection;
  }

  bool hasStackSection() const { return StackSection.has_value(); }
  auto &getStackSection() const { return *StackSection; }
  bool hasROMSection() const { return ROMSection.has_value(); }
  auto &getROMSection() const { return *ROMSection; }
  bool hasExternalStack() const { return ExternalStack; }
  bool stackEnabled() const { return hasStackSection() || hasExternalStack(); }

  MCRegister getStackPointer() const { return StackPointer; }

  auto getStackTop() const {
    assert(hasStackSection() && "No stack section");
    auto &Stack = StackSection.value();
    return Stack.VMA + Stack.Size;
  }

  bool exportedNamesMangled() const { return MangleExportedNames; }

  StringRef getEntryPointName() const { return EntryPointName; }

  bool followTargetABI() const { return FollowTargetABI; }

private:
  friend RootRegPoolWrapper;
  void initializeStackSection(const SnippyProgramSettings &Settings);
  void initializeSelfcheckSection(const SnippyProgramSettings &Settings);
  void initializeUtilitySection(const SnippyProgramSettings &Settings);
  void initializeROMSection(const SnippyProgramSettings &Settings);

  LLVMState *State = nullptr;
  RegisterGenerator *RegGen = nullptr;
  std::vector<RegPool> RegPoolsStorage;

  const OpcodeCache *OpCC = nullptr;
  std::unique_ptr<Linker> PLinker;

  constexpr static auto SmallStringDefaultSize = 16;
  constexpr static auto SCStride = 16u;
  constexpr static auto kPageSize = 0x1000u;

  std::optional<SectionDesc> ROMSection;
  std::optional<SectionDesc> SelfcheckSection;
  std::optional<SectionDesc> StackSection;
  std::optional<SectionDesc> UtilitySection;

  MCRegister StackPointer;
  bool MangleExportedNames;
  std::string EntryPointName;
  bool ExternalStack;
  bool FollowTargetABI;
};

} // namespace snippy
} // namespace llvm
