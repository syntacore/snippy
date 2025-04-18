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
  LINK_LIBS PRIVATE
    $<BUILD_LOCAL_INTERFACE:LLVMSnippyGenerator>
  )
