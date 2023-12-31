add_subdirectory(Config)
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
  SnippyGenerator
  )

add_llvm_library(LLVMSnippy
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  BranchRelaxatorPass.cpp
  BlockGenPlanningPass.cpp
  CFGeneratorPass.cpp
  CFGPrinterPass.cpp
  CFPermutationPass.cpp
  FillExternalFunctionsStubsPass.cpp
  FlowGenerator.cpp
  FillExternalFunctionsStubsPass.cpp
  FunctionGeneratorPass.cpp
  GeneratorContextPass.cpp
  InstructionsPostProcessPass.cpp
  LoopAlignmentPass.cpp
  LoopCanonicalizationPass.cpp
  LoopLatcherPass.cpp
  PassManagerWrapper.cpp
  PrintMachineInstrsPass.cpp
  PrologueEpilogueInsertionPass.cpp
  RegsInitInsertionPass.cpp
  ReserveRegsPass.cpp
  RootRegPoolWrapperPass.cpp
  DEPENDS
  intrinsics_gen
  )
