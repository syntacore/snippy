set(LLVM_TARGET_DEFINITIONS SupportOptions.td)
tablegen(SNIPPY SnippySupportOptions.inc -gen-options -I
         ${LLVM_SNIPPY_HEADERS_DIR})
add_public_tablegen_target(LLVMSnippySupportOptionsTableGen)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Config.inc.in
               ${CMAKE_CURRENT_BINARY_DIR}/Config.inc)
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

target_include_directories(LLVMSnippySupport PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(LLVMSnippySupport LLVMSnippySupportOptionsTableGen)
