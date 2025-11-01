//===-- SnippyModule.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "snippy/Generator/GenResult.h"
#include "snippy/Generator/GlobalsPool.h"
#include "snippy/Generator/Linker.h"
#include "snippy/Generator/RegisterGenerator.h"
#include "snippy/GeneratorUtils/LLVMState.h"
#include "snippy/GeneratorUtils/RegisterPool.h"
#include "snippy/Target/Target.h"

#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/Debug.h"

namespace llvm {
class TargetSubtargetInfo;
} // namespace llvm

namespace llvm {
namespace snippy {

struct GeneratorResult {
  enum class Type {
    RELOC,       // relocatable elf image
    LEGACY_EXEC, // executable with legacy header layout
    EXEC,        // executable elf image
    DYN          // dynamic (shared object) elf image
  } GenType;
  std::string SnippetImage;
  std::string LinkerScript;
};

struct ObjectMetadata {
  size_t EntryPrologueInstrCnt = 0;
  size_t EntryEpilogueInstrCnt = 0;
};

class RegisterGenerator;
class GlobalsPool;
class MonoAllocatableSection;
class OpcodeCache;
class MemoryManager;
class ProgramConfig;
class RootRegPoolWrapper;

struct ObjectFile final {
  SmallString<32> Object;
};

extern template class GenResultT<ObjectFile>;
extern template class GenResultT<ObjectMetadata>;

class SnippyModule final : private Module {
  using Module::setTargetTriple;

public:
  SnippyModule(LLVMState &State, StringRef Name);

  using Module::getName;
  const auto &getModule() const { return static_cast<const Module &>(*this); }
  auto &getModule() { return static_cast<Module &>(*this); }
  static SnippyModule &fromModule(Module &M) {
    return static_cast<SnippyModule &>(M);
  }

  template <typename T> bool hasGenResult() const {
    return llvm::any_of(Results,
                        [](auto &&Res) { return Res->template isA<T>(); });
  }

  template <typename T> T &getGenResult() {
    auto Found = llvm::find_if(
        Results, [](auto &&Res) { return Res->template isA<T>(); });
    assert(Found != Results.end());
    return static_cast<GenResultT<T> &>(**Found).Value;
  }

  template <typename T> const T &getGenResult() const {
    auto Found = llvm::find_if(
        Results, [](auto &&Res) { return Res->template isA<T>(); });
    assert(Found != Results.end());
    return static_cast<GenResultT<T> &>(**Found).Value;
  }

  template <typename T, typename... Types> T &addGenResult(Types &&...Args) {
    assert(!hasGenResult<T>());
    auto &NewResult = *Results.emplace_back(
        std::make_unique<GenResultT<T>>(std::forward<Types>(Args)...));
    return static_cast<GenResultT<T> &>(NewResult).Value;
  }

  template <typename T, typename... Types> T &getOrAddResult(Types &&...Args) {
    if (hasGenResult<T>())
      return getGenResult<T>();
    return addGenResult<T>(std::forward<Types>(Args)...);
  }

  auto &getMMI() const { return MMI; }

  const auto &getLLVMState() const { return State; }

  using PassInserter = std::function<void(PassManagerWrapper &)>;

  void generateObject(const PassInserter &BeforePrinter,
                      const PassInserter &AfterPrinter);

  bool haveGeneratedObject() const { return hasGenResult<ObjectFile>(); }

  const auto &getGeneratedObject() const {
    return getGenResult<ObjectFile>().Object;
  }

private:
  LLVMState &State;
  std::unique_ptr<MCContext> Context;
  std::unique_ptr<MachineModuleInfoWrapperPass> MMIWP;
  MachineModuleInfo &MMI;
  std::unique_ptr<PassManagerWrapper> PPM;
  std::vector<std::unique_ptr<GenResult>> Results;
};

// How does the static stack work?
// It reserves a part of stack for every function and each of them saves values
// to the predefined addresses in any call.
//
// 1. MFi started spilling on stack. The SPAddrLocal is decremented.
//  ________
// | MFi-1  |
// |========|
// |  MFi   |
// |________|
// |________| <- SPAddrLocal
//
// 2. MFi finished spilling and SPAddrGlobal was set at SPAddrLocal.
//  ________
// | MFi-1  |
// |========|
// |  MFi   |
// |  ...   |
// |________| <- SPAddrLocal, SPAddrGlobal
//
// 3. MFi started reloading from stack
//  ________
// | MFi-1  |
// |========|
// |  MFi   |
// |  ...   |
// |________| <- SPAddrLocal
// |  ...   |
// |________| <- SPAddrGlobal
//
// 4. MFi+1 started spilling on stack. SPAddrLocal is set to SPAddrGlobal.
//    Step 1 is repeated.
//  ________
// | MFi-1  |
// |========|
// |  MFi   |
// |========| <- SPAddrGlobal, SPAddrLocal
// | MFi+1  |
// |________|
class StaticStackContext final {
public:
  void reset() {
    SPAddrLocal = std::nullopt;
    RegWithSPAddrLocal = std::nullopt;
  }
  void setSPAddrGlobal(size_t Addr) { SPAddrGlobal = Addr; }
  void passSPAddr() {
    SPAddrGlobal = getSPAddrLocal();
    RegWithSPAddrLocal = std::nullopt;
  }
  void setSPAddrLocal(size_t SPAddr) { SPAddrLocal = SPAddr; }
  void setRegWithSPAddrLocal(MCRegister Reg) { RegWithSPAddrLocal = Reg; }
  void resetRegWithSPAddrLocal() { RegWithSPAddrLocal = std::nullopt; }

  bool isSPInReg() const { return RegWithSPAddrLocal != std::nullopt; }
  size_t getSPAddrGlobal() const {
    assert(SPAddrGlobal);
    return *SPAddrGlobal;
  }
  size_t getSPAddrLocal() const {
    assert(SPAddrLocal);
    return *SPAddrLocal;
  }
  MCRegister getRegWithSPAddrLocal() const {
    assert(isSPInReg());
    return *RegWithSPAddrLocal;
  }

private:
  std::optional<size_t> SPAddrLocal;
  std::optional<size_t> SPAddrGlobal;

  // This field is needed to optimize the stack operation. Here is the register
  // in which SPAddrLocal is written. This register can be used to access the
  // stack.
  std::optional<MCRegister> RegWithSPAddrLocal;
};

class SnippyProgramContext final {
public:
  SnippyProgramContext(LLVMState &State, RegisterGenerator &RegGen,
                       std::vector<RegPool> Pools, const OpcodeCache &OpCc,
                       const ProgramConfig &Settings);

  SnippyProgramContext(SnippyProgramContext &&) = default;
  SnippyProgramContext &operator=(SnippyProgramContext &&) = default;

  ~SnippyProgramContext();

  friend RootRegPoolWrapper;

  const auto &getConfig() const { return *Cfg; }
  auto &getLLVMState() const { return *State; }

  StringRef getOutputSectionName(const Function &F) const {
    return F.hasSection() ? F.getSection() : ".text";
  }

  auto getOutputSectionFor(const Function &F) const {
    auto SectionName = getOutputSectionName(F);
    assert(PLinker->sections().hasOutputSectionFor(SectionName));
    return PLinker->sections().getOutputSectionFor(SectionName).Desc;
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

  // May fail if no appropriate section found or if all suitable section
  // are already taken. In such case Failure is returned.
  Expected<GlobalsPool &> getOrAddGlobalsPoolFor(Module &M);

  // Wrapper helper for method above. Get llvm::Module from SnippyModule
  Expected<GlobalsPool &> getOrAddGlobalsPoolFor(SnippyModule &M);

  // Terminates on error printing 'OnError'
  // message.
  GlobalsPool &getOrAddGlobalsPoolFor(Module &M, StringRef OnError);

  // Wrapper helper for method above.
  GlobalsPool &getOrAddGlobalsPoolFor(SnippyModule &M, StringRef OnError);

  Expected<GeneratorResult> generateELF(ArrayRef<const SnippyModule *> Modules,
                                        GeneratorResult::Type GenType,
                                        bool NoRelax) const;

  Linker &getLinker() const { return *PLinker; }
  RegisterGenerator &getRegGen() const { return *RegGen; }
  const auto &getOpcodeCache() const { return *OpCC; }

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

  auto &preserveCallerSavedGroups() const { return PreserveCallerSavedGroups; }

  // When an arbitrary register is used as a stack pointer and this register
  // must be preserved across snippy function call (callee-saved), we have to
  // save it to the stack before start using it.
  bool shouldSpillStackPointer() const;

  const IRegisterState &
  getInitialRegisterState(const TargetSubtargetInfo &ST) const;

  bool hasProgramStateSaveSpace() const { return PGSK.get(); }
  const auto &getProgramStateSaveSpace() const { return *PGSK; }
  auto &getProgramStateSaveSpace() { return *PGSK; }

  const MonoAllocatableSection &getROMSectionManager() const {
    return *ROMSectionManager;
  }
  MonoAllocatableSection &getROMSectionManager() { return *ROMSectionManager; }

  TargetGenContextInterface &getTargetContext() const {
    assert(TargetContext && "no target context");
    return *TargetContext;
  }

  StaticStackContext &getStaticStack() const {
    assert(StaticStack && "no static stack");
    return *StaticStack;
  }

  // TODO: We should define a subset of Config that is enough for
  // TargetContext initialization.
  void createTargetContext(const Config &Cfg, const TargetSubtargetInfo &STI);

private:
  friend RootRegPoolWrapper;
  void initializeStackSection(const ProgramConfig &Settings);
  void initializeSelfcheckSection(const ProgramConfig &Settings);
  void initializeUtilitySection(const ProgramConfig &Settings);
  void initializeROMSection(const ProgramConfig &Settings);
  void initializeStaticStack(const ProgramConfig &Settings);

  const ProgramConfig *Cfg;
  LLVMState *State = nullptr;
  RegisterGenerator *RegGen = nullptr;
  std::vector<RegPool> RegPoolsStorage;

  const OpcodeCache *OpCC = nullptr;
  std::unique_ptr<Linker> PLinker;

  constexpr static auto SmallStringDefaultSize = 16;

  std::optional<SectionDesc> ROMSection;
  std::optional<SectionDesc> SelfcheckSection;
  std::optional<SectionDesc> StackSection;
  std::optional<SectionDesc> UtilitySection;
  std::unique_ptr<MonoAllocatableSection> ROMSectionManager;
  std::unique_ptr<ProgramGlobalStateKeeper> PGSK;
  std::map<Module *, std::unique_ptr<GlobalsPool>> PerModuleGPs;
  std::unique_ptr<TargetGenContextInterface> TargetContext;
  std::unique_ptr<StaticStackContext> StaticStack;

  MCRegister StackPointer;
  bool MangleExportedNames;
  std::string EntryPointName;
  bool ExternalStack;
  bool FollowTargetABI;
  std::vector<std::string> PreserveCallerSavedGroups;

  // TODO: it would be nice to be able to initialize it right away, but
  // currently it depends on TargetSubtargetInfo which is diffucult to get
  // before Module and first MachineFunction creation.
  std::string InitialRegYamlFile;
  mutable std::unique_ptr<IRegisterState> InitialMachineState = nullptr;
};

} // namespace snippy
} // namespace llvm
