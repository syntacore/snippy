//===-- ActiveImmutablePass.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Pass.h"

namespace llvm {
namespace snippy {

/// Common dynamic interface of ActiveImmutablePass.
/// Do not inherit your own passes from it, instead
/// inherit them from ActiveImmutablePass<> template
/// specializations.
class ActiveImmutablePassInterface {
public:
  // Called by PassManagerWrapper.
  virtual Pass *getAsPass() = 0;
  virtual ImmutablePass *createStoragePass() = 0;

  virtual ~ActiveImmutablePassInterface() = default;

  template <typename Derived>
  static void initialize(PassRegistry &PR, StringRef Arg, StringRef Desc,
                         bool IsCfgOnly) {
    initializeImpl(PR, getUnique(Derived::ID),
                   PassInfo::NormalCtor_t(callDefaultCtor<Derived>), Arg, Desc,
                   IsCfgOnly, false);
    initializeImpl(
        PR, Derived::ID, nullptr,
        StringStorage.emplace_back((Twine(Arg) + "-storage").str()).c_str(),
        StringStorage.emplace_back((Twine(Desc) + " Storage").str()).c_str(),
        false, true);
  }

private:
  static void initializeImpl(PassRegistry &PR, char &ID,
                             PassInfo::NormalCtor_t Ctor, StringRef Arg,
                             StringRef Desc, bool IsCfgOnly, bool IsAnalysis) {
    PassInfo *PI = new PassInfo(Desc, Arg, &ID, Ctor, IsCfgOnly, IsAnalysis);
    PR.registerPass(*PI, true);
  }

protected:
  static char &getUnique(char &ID) {
    auto UniqueLoc = new char(0);
    auto &&[Inserted, _] = IDStash.emplace(&ID, UniqueLoc);
    return *Inserted->second;
  }

private:
  static std::unordered_map<char *, std::unique_ptr<char>> IDStash;
  static std::vector<std::string> StringStorage;
};

/// \template ActiveImmutablePass
///
/// \brief implements generic "run-once" pass with arbitrary pack of
///        produced analysises.
///
/// \param Base - llvm pass type to inherit from. Supported types: ModulePass,
///                MachineFunctionPass.
/// \pack  AnalysisTs - zero or more types of analysises producesd by this pass.
template <typename Base, typename... AnalysisTs>
class ActiveImmutablePass : public Base, public ActiveImmutablePassInterface {
public:
  /// To implement your ActiveImmutablePass, declare a class
  /// that inherits from this template specialization. Example:
  ///
  /// class MyPass: public ActiveImmutablePass<ModulePass, MyAnalysis>{
  /// public:
  ///    // same as regular pass
  ///    static char ID;
  ///    MyPass(): ActiveImmutablePass<ModulePass, MyAnalysis>(ID){};
  ///
  ///    // Same as regular ModulePass
  ///    bool runOnModule(Module& M) override{
  ///      // Mutable lvalue reference to instance of MyAnalysis.
  ///      MyAnalysis& MA = get<MyAnalysis>();
  ///      // Here pass may write to MA, those modifications
  ///      // can be observed by later passes.
  ///      ...
  ///    }
  /// };
  ///
  /// In some other pass that may want to read MyAnalysis written by this pass
  /// you should:
  ///
  /// class MyAnotherPass: public MachineFunctionPass {
  /// public:
  ///   void getAnalysisUsage(AnalysisUsage& AU) override {
  ///     // Add MyPass dependency as usual.
  ///     AU.addRequired<MyPass>();
  ///     ...
  ///   }
  ///
  ///   bool runOnMachineFunction(MachineFunction &MF) {
  ///     // Get MyAnalysis written by MyPass.
  ///     const MyAnalysis& MA = getAnalysis<MyPass>().get<MyAnalysis>();
  ///     ...
  ///   };
  /// };
  ///
  ///  NOTE #1: This pass as analysis is never invalidated -  it is not
  ///           nessesary to mark it as preserved by another passes.
  ///  NOTE #2: create<MyActiveImmutablePassName>() function must
  ///           return ActiveImmutablePassInterface*. The pointer of
  ///           same type must be passed to PassManagerWrapper::add()
  ///  NOTE #3: This pass can only be added to snippy::PassManagerWrapper,
  ///           do not use it with standard llvm pass manager.
  ///  NOTE #4: This pass must not be added more than once to same
  ///           pass manager instance.
  ///  NOTE #5: SNIPPY_INITIALIZE_PASS() macro must be used instead of
  ///           INITIALIZE_PASS() for every ActiveImmutablePass.
  ///  NOTE #6: It is safe to make a:
  ///                ModulePass -> ActiveImmutablePass<MachineFunction>
  ///           dependency. Individual analysis instance for specific
  ///           function can be retrieved by:
  ///                getAnalysis<SomeMFPass>().get<SomeMFAnalysis>(A);
  ///           where A - is either a Function& or MachineFunction&.
  ActiveImmutablePass(char &ID) : Base(getUnique(ID)), RealID(&ID){};

  // Called by PassManagerWrapper.
  ImmutablePass *createStoragePass() override {
    return new DecoyPass(this, *RealID);
  }
  Pass *getAsPass() override { return static_cast<Pass *>(this); }

  bool doInitialization(Module &) override {
    // Sanity check. Fails if this pass is not initialized or it's initializer
    // was not generated by SNIPPY_INITIALIZE_PASS macro.
    auto *DecoyPI = PassRegistry::getPassRegistry()->getPassInfo(RealID);
    auto *ThisPI =
        PassRegistry::getPassRegistry()->getPassInfo(Pass::getPassID());
    assert(DecoyPI && ThisPI &&
           "ActiveImmutablePass happens to be uninitialized");
    auto DecoyPassName = DecoyPI->getPassName();
    auto ThisPassName = ThisPI->getPassName();
    assert(DecoyPassName.consume_back(" Storage") &&
           "please, use SNIPPY_INITIALIZE_PASS()");

    assert(ThisPassName == DecoyPassName &&
           "please, use SNIPPY_INITIALIZE_PASS()");
    return false;
  }
  // Decoy pass is a legit immutable pass, so it's
  // analysis info is never invalidated. It will be registered
  // with ID of main pass in PassManager. That way,
  // getAnalysis<MainPass>() will refer to Decoy pass.
  class DecoyPass : public ImmutablePass {
  public:
    DecoyPass(Pass *RealPass, char &ID)
        : ImmutablePass(ID), RealPass(RealPass),
          Name((Twine(RealPass->getPassName()) + " Storage").str()){};
    StringRef getPassName() const override { return Name; }

    // A hack to get a pointer to real path from decoy.
    void *getAdjustedAnalysisPointer(AnalysisID ID) override {
      return RealPass;
    }

  private:
    Pass *RealPass;
    std::string Name;
  };

  static constexpr auto IsFunctionAnalysis =
      std::is_base_of_v<FunctionPass, Base>;
  // Based on the type of base pass, choose apropriate
  // storage container for analysis instance. If it is a ModulePass,
  // simple unique_ptr is enough. However, if it is a FunctionPass
  // or any of its derivatives, distinct instance of analysis for each
  // function is required - use a hash table (Funtion <-> AnalysisInstance).
  template <typename AnalysisT>
  using AnalysisStorage = std::conditional_t<
      IsFunctionAnalysis,
      std::unordered_map<const Function *, std::unique_ptr<AnalysisT>>,
      std::unique_ptr<AnalysisT>>;

  template <typename FH>
  using ModulePassOnly =
      std::enable_if_t<!IsFunctionAnalysis && std::is_same_v<int, FH>, bool>;
  template <typename FH, typename Expected>
  using FunctionPassOnly =
      std::enable_if_t<IsFunctionAnalysis && std::is_same_v<Expected, FH>,
                       bool>;

  // Get Analysis of type AT. (ModulePass only).
  template <typename AT, typename FH = int, ModulePassOnly<FH> = true>
  AT &get(const FH & = 0) {
    static_assert(!IsFunctionAnalysis);
    auto &ATStorage = std::get<AnalysisStorage<AT>>(Storage);
    if (!ATStorage)
      ATStorage = std::make_unique<AT>();
    return *ATStorage;
  }

  // Get Analysis of type AT. (ModulePass only). const version.
  template <typename AT, typename FH = int, ModulePassOnly<FH> = true>
  const AT &get(const FH & = 0) const {
    static_assert(!IsFunctionAnalysis);
    auto &ATStorage = std::get<AnalysisStorage<AT>>(Storage);
    assert(ATStorage && "Trying to read uninitialized analysis");
    return *ATStorage;
  }

  // Get Analysis of type AT for Function F. (FunctionPass only).
  template <typename AT, typename FH, FunctionPassOnly<FH, Function> = true>
  AT &get(const FH &F) {
    static_assert(IsFunctionAnalysis);
    auto &ATStorage = std::get<AnalysisStorage<AT>>(Storage);
    if (!ATStorage.count(&F))
      ATStorage.emplace(&F, std::make_unique<AT>());
    return *ATStorage.at(&F);
  }

  // Get Analysis of type AT for Function F. (FunctionPass only). const version.
  template <typename AT, typename FH, FunctionPassOnly<FH, Function> = true>
  const AT &get(const FH &F) const {
    static_assert(IsFunctionAnalysis);
    auto &ATStorage = std::get<AnalysisStorage<AT>>(Storage);
    assert(ATStorage.count(&F) && "Trying to read uninitialized analysis");
    return *ATStorage.at(&F);
  }

  // Get Analysis of type AT for MachineFunction MF. (FunctionPass only).
  template <typename AT, typename FH,
            FunctionPassOnly<FH, MachineFunction> = true>
  AT &get(const FH &MF) {
    static_assert(IsFunctionAnalysis);
    return get<AT>(MF.getFunction());
  }

  // Get Analysis of type AT for MachineFunction MF. (FunctionPass only). (const
  // version)
  template <typename AT, typename FH,
            FunctionPassOnly<FH, MachineFunction> = true>
  const AT &get(const FH &MF) const {
    static_assert(IsFunctionAnalysis);
    return get<AT>(MF.getFunction());
  }

private:
  char *RealID;
  std::tuple<AnalysisStorage<AnalysisTs>...> Storage;
};

/// All ActiveImmutablePasses MUST be initialized via this macro!!!
#define SNIPPY_INITIALIZE_PASS(passName, arg, name, cfg)                       \
  static void initialize##passName##PassOnce(PassRegistry &Registry) {         \
    llvm::snippy::ActiveImmutablePassInterface::initialize<passName>(          \
        Registry, arg, name, cfg);                                             \
  }                                                                            \
  static llvm::once_flag Initialize##passName##PassFlag;                       \
  void llvm::initialize##passName##Pass(PassRegistry &Registry) {              \
    llvm::call_once(Initialize##passName##PassFlag,                            \
                    initialize##passName##PassOnce, std::ref(Registry));       \
  }
} // namespace snippy
} // namespace llvm
