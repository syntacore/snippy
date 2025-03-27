add_llvm_library(
  LLVMSnippySupport
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  APIntSampler.cpp
  DiagnosticInfo.cpp
  DynLibLoader.cpp
  Error.cpp
  OpcodeCache.cpp
  OpcodeGenerator.cpp
  Options.cpp
  RandUtil.cpp
  Utils.cpp
  YAMLUtils.cpp)

if(LLVM_SNIPPY_NEEDS_LIBSTDCPP_FS)
  target_link_libraries(LLVMSnippySupport PRIVATE stdc++fs)
endif()
