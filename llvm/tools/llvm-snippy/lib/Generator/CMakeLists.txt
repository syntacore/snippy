set(LLVMSnippyGeneratorSources
  Backtrack.cpp
  CallGraphState.cpp
  CFPermutation.cpp
  CFPermutationPass.cpp
  FunctionGeneratorPass.cpp
  FPRNaNIdentifier.cpp
  GeneratorContext.cpp
  GeneratorContextPass.cpp
  GenerationLimit.cpp
  GenerationUtils.cpp
  Generation.cpp
  InstructionGeneratorPass.cpp
  Interpreter.cpp
  Linker.cpp
  IntervalsToVerify.cpp
  Policy.cpp
  RegisterGenerator.cpp
  RootRegPoolWrapperPass.cpp
  SimRunner.cpp
  SimulatorContextWrapperPass.cpp
  SnippyFunctionMetadata.cpp
  MemoryManager.cpp
  OperandsReinitializationPolicy.cpp
  OperandsReinitializationValueSource.cpp
  SnippyModule.cpp
  TopMemAccSampler.cpp
  RandomMemAccSampler.cpp
)


add_llvm_library(LLVMSnippyGenerator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippyGeneratorSources}
  )

if(LLVM_SNIPPY_NEEDS_LIBSTDCPP_FS)
  target_link_libraries(LLVMSnippyGenerator PRIVATE stdc++fs)
endif()

target_link_libraries(LLVMSnippyGenerator
  PRIVATE LLVMSnippyTarget
  PUBLIC LLVMSnippyConfig LLVMSnippyGeneratorUtils
  SnippyHeaders
  )
