add_llvm_library(
  LLVMSnippySupport
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  DiagnosticInfo.cpp
  DynLibLoader.cpp
  OpcodeCache.cpp
  Options.cpp
  OpcodeGenerator.cpp
  RandUtil.cpp
  Utils.cpp
  YAMLUtils.cpp
  APIntSampler.cpp)

if(LLVM_SNIPPY_NEEDS_LIBSTDCPP_FS)
  target_link_libraries(LLVMSnippySupport PRIVATE stdc++fs)
endif()
