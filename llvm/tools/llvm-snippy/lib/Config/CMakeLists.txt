add_llvm_library(LLVMSnippyConfig
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
    Config.cpp
    OpcodeHistogram.cpp
    Branchegram.cpp
    MemoryScheme.cpp
    RegisterHistogram.cpp
    SerDesUtils.cpp
    PluginWrapper.cpp
    CallGraphLayout.cpp
    MemorySchemePluginWrapper.cpp
    )

target_link_libraries(LLVMSnippyConfig PRIVATE LLVMSnippySupport)
