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
  PluginMemAccSampler.cpp
  RandomMemAccSampler.cpp
)


add_llvm_library(LLVMSnippyGenerator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippyGeneratorSources}
)

target_link_libraries(LLVMSnippyGenerator
  PRIVATE LLVMSnippyTarget)
