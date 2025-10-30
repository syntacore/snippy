set(LLVMSnippyGeneratorUtilsSources
  LLVMState.cpp
  RegisterPool.cpp
)

add_llvm_library(LLVMSnippyGeneratorUtils
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippyGeneratorUtilsSources}
  )

target_link_libraries(LLVMSnippyGeneratorUtils
  PRIVATE LLVMSnippy
  PUBLIC LLVMSnippyConfig
  SnippyHeaders
  )
