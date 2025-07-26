add_subdirectory(Config)
add_subdirectory(OptionCategory)
add_subdirectory(Simulator)
add_subdirectory(Support)
add_subdirectory(Target)
add_subdirectory(Generator)

set(LLVM_SNIPPY_TARGETS "${LLVM_SNIPPY_TARGETS} ${TARGETS_TO_APPEND}" PARENT_SCOPE)

set(LLVM_LINK_COMPONENTS
  Analysis
  CodeGen
  Core
  ExecutionEngine
  GlobalISel
  MC
  MCDisassembler
  MCJIT
  MCParser
  Object
  ObjectYAML
  RuntimeDyld
  Support

  SnippyConfig
  SnippySupport
  SnippyTarget
  )
set(LLVMSnippySources
  BranchRelaxatorPass.cpp
  BlockGenPlanningPass.cpp
  BlockGenPlanWrapperPass.cpp
  PreserveRegsInsertionPass.cpp
  CFGeneratorPass.cpp
  CFGPrinter.cpp
  ConsecutiveLoopsVerifierPass.cpp
  FillExternalFunctionsStubsPass.cpp
  FlowGenerator.cpp
  FillExternalFunctionsStubsPass.cpp
  FunctionDistributePass.cpp
  InitializePasses.cpp
  InstructionsPostProcessPass.cpp
  PostGenVerifierPass.cpp
  LoopAlignmentPass.cpp
  LoopCanonicalizationPass.cpp
  LoopLatcherPass.cpp
  TrackLivenessPass.cpp
  MemAccessDumperPass.cpp
  PassManagerWrapper.cpp
  PrintMachineInstrsPass.cpp
  PrologueEpilogueInsertionPass.cpp
  RegsInitInsertionPass.cpp
  ReserveRegsPass.cpp
  )


add_llvm_library(LLVMSnippy
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippySources}
  DEPENDS
  intrinsics_gen
  LLVMSnippyDriverOptionsTableGen
  LINK_LIBS PRIVATE
    $<BUILD_LOCAL_INTERFACE:LLVMSnippyGenerator>
  )

set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(SNIPPY SnippyDriverOptions.inc -gen-options -I
         ${LLVM_SNIPPY_HEADERS_DIR})
tablegen(SNIPPY SnippyDriverOptionsStruct.inc -gen-options-struct -I
         ${LLVM_SNIPPY_HEADERS_DIR})
add_public_tablegen_target(LLVMSnippyDriverOptionsTableGen)
target_include_directories(llvm-snippy PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(LLVMSnippy PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
