set(LLVMSnippyConfigSources
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


add_llvm_library(LLVMSnippyConfig
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippyConfigSources}
    )

target_link_libraries(LLVMSnippyConfig PRIVATE LLVMSnippySupport)
