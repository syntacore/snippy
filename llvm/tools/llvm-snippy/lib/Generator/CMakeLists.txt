add_llvm_library(LLVMSnippyGenerator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  Backtrack.cpp
  CallGraphState.cpp
  GeneratorContext.cpp
  GenerationLimit.cpp
  GenerationUtils.cpp
  Generation.cpp
  Interpreter.cpp
  Linker.cpp
  LLVMState.cpp
  IntervalsToVerify.cpp
  Policy.cpp
  RegisterGenerator.cpp
  RegisterPool.cpp
  SimRunner.cpp
  MemoryManager.cpp
  ImmediateHistogram.cpp
)

target_link_libraries(LLVMSnippyGenerator
  PRIVATE LLVMSnippyTarget)
