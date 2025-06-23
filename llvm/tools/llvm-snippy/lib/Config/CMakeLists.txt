set(LLVMSnippyConfigSources
    Config.cpp
    OpcodeHistogram.cpp
    Branchegram.cpp
    BurstGram.cpp
    MemoryScheme.cpp
    RegisterHistogram.cpp
    PluginWrapper.cpp
    FPUSettings.cpp
    CallGraphLayout.cpp
    ImmediateHistogram.cpp
    Valuegram.cpp
    )


add_llvm_library(LLVMSnippyConfig
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippyConfigSources}
  LINK_LIBS PRIVATE LLVMSnippySupport LLVMSnippyOptionCategory
  PUBLIC SnippyHeaders
    )

set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(SNIPPY SnippyConfigOptions.inc -gen-options -I
         ${LLVM_SNIPPY_HEADERS_DIR})
tablegen(SNIPPY SnippyConfigOptionsStruct.inc -gen-options-struct -I
         ${LLVM_SNIPPY_HEADERS_DIR})
add_public_tablegen_target(LLVMSnippyConfigOptionsTableGen)
add_dependencies(SnippyHeaders LLVMSnippyConfigOptionsTableGen)
target_include_directories(LLVMSnippyConfig
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  )
