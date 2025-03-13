set(LLVMSnippyGeneratorSources
  Backtrack.cpp
  CallGraphState.cpp
  CFPermutation.cpp
  CFPermutationPass.cpp
  FunctionGeneratorPass.cpp
  GeneratorContext.cpp
  GeneratorContextPass.cpp
  GenerationLimit.cpp
  GenerationUtils.cpp
  Generation.cpp
  InstructionGeneratorPass.cpp
  Interpreter.cpp
  Linker.cpp
  LLVMState.cpp
  IntervalsToVerify.cpp
  Policy.cpp
  RegisterGenerator.cpp
  RegisterPool.cpp
  RootRegPoolWrapperPass.cpp
  SimRunner.cpp
  SimulatorContextWrapperPass.cpp
  SnippyFunctionMetadata.cpp
  SnippyModule.cpp
  MemoryManager.cpp
  ImmediateHistogram.cpp
  TopMemAccSampler.cpp
  RandomMemAccSampler.cpp
)


# FIXME: we make this an object library to fix several internal circular
# dependencies and linkage issues. This is a workaround and should be removed
# after we refactor our libraries structure
add_library(LLVMSnippyGenerator
  OBJECT
  EXCLUDE_FROM_ALL
  ${LLVMSnippyGeneratorSources}
)
llvm_update_compile_flags(LLVMSnippyGenerator)

if(LLVM_SNIPPY_NEEDS_LIBSTDCPP_FS)
  target_link_libraries(LLVMSnippyGenerator PRIVATE stdc++fs)
endif()

target_link_libraries(LLVMSnippyGenerator
  PRIVATE LLVMSnippyTarget)
