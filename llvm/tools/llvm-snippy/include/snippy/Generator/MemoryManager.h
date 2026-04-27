//===-- MemoryManager.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include "snippy/Config/MemoryScheme.h"
#include "snippy/Config/OpcodeHistogram.h"
#include "snippy/Target/Target.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
namespace snippy {
namespace planning {

class InstructionGenerationContext;

}
class Linker;
class GeneratorContext;
class Interpreter;
class LLVMState;
class SnippyProgramContext;

struct SectionData {
  SectionDesc Desc;
  // All addresses are calculated from the start of a section
  MemoryMap MemMap;
  std::unordered_set<MemAddr> AddressesToInit;
  // This flag shows that section can be initializaed after execution
  bool HasLaterInit = false;

  SectionData(const SectionDesc &Desc, MemoryMap MemMap = MemoryMap{},
              bool HasLaterInit = false)
      : Desc{Desc}, MemMap{MemMap}, HasLaterInit{HasLaterInit} {}

  SectionData(const std::vector<MemoryUnit> &SectionMem,
              const SectionDesc &NewDesc)
      : Desc{NewDesc} {
    std::vector<MemAddr> MemOffset(SectionMem.size());
    std::iota(MemOffset.begin(), MemOffset.end(), 0u);
    for (auto [MemVal, Addr] : zip(SectionMem, MemOffset))
      MemMap.emplace(Addr, MemVal);
  }

  bool isAccessValid(MemAddr Addr, size_t AccessSize) const {
    return (Addr >= Desc.VMA) && (Addr + AccessSize <= Desc.VMA + Desc.Size);
  }
};

struct GlobalCodeFlowInfo;

// Memory generation process:
//  1) general entities:
//    - MemState - internal representation of the initial memory of the snippet
//    - SectionData - representation of the memory to initialize;
//                    section might be write only,
//                    which means that it is initialized after code generation
//                    and, threfore, selfcheck;
//                    this means that the content of such sections won't affect
//                    a final trace
//  2) interaction with other classes:
//    - `SimRunner` initializaes interpreters memory with the state
//      received from MemoryManager
//      (in order to load them from ELF to model during final execution)
//    - `Linker` gets some input sections from MemoryManager.
//      specifier, Linker should have input sections for them.
//    - `Module` is filled with global constants corresponds to MemState.
//      MemoryManager emmits global constants with internal linkage
//      to the corresponds sections.
//    - `Pass manager`: a separate Module with additional pipeline is created
//      in order to generate random function without any optimizations.
//      Auxiliary pipeline is created in
//      createExternalMemInitRoutine() function.
//    - `MemoryScheme` is used to randomly sample the scheme
//      when memory is initialized with addresses.
//  3) general pipeline:
//     ... pre-backtrack passes ...
//                |
//                V
//        MemoryInitializerPass:
//          Adds write only selfcheck section to MemState
//          if Full(Full, FullWithAddresses) initialization:
//            -MemState creation
//            -constants emmition
//            -setting of Linker input sections
//            -setting SimRunner state (additional sections)
//          if Runtime initialization:
//            -external call generation
//            -MemState creation
//          if Loads(LoadsOnly, LoadsWithAddresses) initialization:
//            -external call generation
//            -MemState creation
//          Setting interpreter memory
//
//                 |
//                 V
//        InstructionGeneratorPass:
//          if Loads initialization:
//            -marking memory accesses for each load instruction
//          if selfcheck-ref-value-storage=memory:
//            -stores reference values and their addresses
//             in the selfcheck section
//                 |
//                 V
//        LateMemoryInitializationPass:
//          Selfcheck sections materialization and its transfer to the model
//
//        Final image creation:
//          if Runtime initialization:
//            -creation of the external randomizer function
//          if Loads initialization:
//            -creation of the external function
//              with loads to the marked memory
//          if !SkipRuntimeInitialization:
//            -linkage of the main object file and external random function
//            (for the model execution)
//          else:
//            -linking main object file only
//            (external function is altready created by
//            FillExternalFunctionsStubs pass)
//            -setting interpreter state to the MemState
//  4) requirements:
//    - MemState should be created before selfcheck generation
//    - MemorySeed should not be zero,
//      because in this case memory will be zero-initialized
//  5) flags policy:
//    1) Runtime:
//        -enabled if -init-memory=Runtime
//        -if -memory-seed is not specified, memory seed is randomly generated
//    2) Full:
//        -enabled if -init-memory=Full
//        -if -memory-seed is not specified, memory generation is performed
//         using Snippy randomizer, which depends on the general seed
//    3) Loads:
//        -enabled if -init-memory=loads
//        -if -memory-seed is not specified, memory generation is performed
//         using Snippy randomizer, which depends on the general seed
//    4) FullWithAddresses:
//        -enabled if -init-memory=full-with-addresses
//        -memory-seed is ignored and memory is filled with addresses based on
//        provided memory scheme and snippy random generator, which depends on
//        the general seed
//    5) LoadsWithAddresses:
//        -enabled if -init-memory=loads-with-addresses
//        -memory-seed is ignored and memory is filled with addresses based on
//        provided memory scheme and snippy random generator, which depends on
//        the general seed
//    6) selfcheck-ref-value-storage=memory:
//        -stores reference selfcheck values to memory
//        - independent flag

struct MemInitializationPseudoRandom {
  static SectionData getSectionData(const SectionDesc &Desc,
                                    planning::InstructionGenerationContext &IGC,
                                    std::optional<uint64_t> MemorySeed);
};

struct MemInitializationWithAddresses {
  static SectionData getSectionData(const SectionDesc &Desc,
                                    planning::InstructionGenerationContext &IGC,
                                    std::optional<uint64_t> MemorySeed);
};

using ParsedMemoryT = std::vector<std::pair<std::string, MemoryMap>>;

struct ASCIIDumpParser final {
  static ParsedMemoryT parse(StringRef FilePath);
};

class MemoryManager final {
  // memory state after initialization
  std::vector<SectionData> MemState;
  // random function name
  std::string ExternFuncName{"snippy_random"};

public:
  MemoryManager() = default;

  using SectionInitFunc = std::function<SectionData(
      const SectionDesc &, planning::InstructionGenerationContext &,
      std::optional<uint64_t>)>;

  template <typename Init>
  void materializeSectionData(Module &M,
                              planning::InstructionGenerationContext &IGC,
                              std::optional<uint64_t> MemorySeed) {
    materializeSectionDataImpl(M, IGC, Init::getSectionData, MemorySeed);
  }

  template <typename Init>
  void createLoadsRuntimeInit(planning::InstructionGenerationContext &IGC,
                              bool ExternalCallOfMemInitRoutine,
                              std::optional<uint64_t> MemorySeed) {
    createLoadsRuntimeInitImpl(IGC, Init::getSectionData,
                               ExternalCallOfMemInitRoutine, MemorySeed);
  }

  template <typename ParserT>
  void loadMemStateFromFile(StringRef FilePath, GeneratorContext &Ctx) {
    auto ParsedState = ParserT::parse(FilePath);
    assert(MemState.empty());
    createMemState(std::move(ParsedState), Ctx);
  }

  // Creates calls to the external randomizer
  //  in order to initialize all RW sections
  //  and initializes MemState.
  // (each sections requires one call)
  void createFullRuntimeInit(planning::InstructionGenerationContext &IGC,
                             MemorySeedTy MemSeed,
                             bool ExternalCallOfMemInitRoutine);

  void writeValueToWriteOnlyAddr(APInt Val, MemAddr Addr);

  void addWriteOnlySection(const SectionDesc &Desc);

  void materializeWriteOnlySections(Module &M, GeneratorContext &Ctx);

  void markMemAccessToInitialize(const MemAddresses &Addresses,
                                 size_t AccessSize);

  void mangleExternalRandomizer(const Linker &L);

  auto getMemState() const {
    return llvm::make_range(MemState.begin(), MemState.end());
  }

  // Returns name of the external randomizer function.
  std::string getExternalRandomizerName() const { return ExternFuncName; }

  void createMemState(ParsedMemoryT ParsedMem, GeneratorContext &Ctx);

  // Creates section and fills them with the provided initialization function
  void fillSectionWithConstants(SnippyProgramContext &ProgCtx, Module &M,
                                const SectionData &SectData) const;

  void initMemState(planning::InstructionGenerationContext &Ctx,
                    SectionInitFunc InitFunc,
                    std::optional<uint64_t> MemorySeed);

  void createLoadsRuntimeInitImpl(planning::InstructionGenerationContext &IGC,
                                  SectionInitFunc InitFunc,
                                  bool ExternalCallOfMemInitRoutine,
                                  std::optional<uint64_t> MemorySeed);

  void materializeSectionDataImpl(Module &M,
                                  planning::InstructionGenerationContext &Ctx,
                                  SectionInitFunc InitFunc,
                                  std::optional<uint64_t> MemorySeed);

  const Function &createMemInitRoutineExtSymbol(Module &M,
                                                LLVMState &State) const;
};
} // namespace snippy
} // namespace llvm
