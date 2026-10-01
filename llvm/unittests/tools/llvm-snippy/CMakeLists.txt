set(LLVM_LINK_COMPONENTS
  Support
  )

set(snippy_unittests_include ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(snippy_init_gtest_env ${CMAKE_CURRENT_SOURCE_DIR}/src/InitSnippyGTestEnv.cpp)
# FIXME: Here we redefine this variable because the one defined in
# llvm/tools/llvm-snippy/lib/Target/CMakeLists.txt is not accessible
# in unittests/ subdirectory
set(LLVM_SNIPPY_TARGETS_LIST)
if(LLVM_TARGETS_TO_BUILD MATCHES "RISCV" OR LLVM_EXPERIMENTAL_TARGETS_TO_BUILD MATCHES "RISCV")
  list(APPEND LLVM_SNIPPY_TARGETS_LIST RISCV)
endif()
if(LLVM_TARGETS_TO_BUILD MATCHES "AArch64" OR LLVM_EXPERIMENTAL_TARGETS_TO_BUILD MATCHES "AArch64")
  list(APPEND LLVM_SNIPPY_TARGETS_LIST AArch64)
endif()
if(LLVM_TARGETS_TO_BUILD MATCHES "X86" OR LLVM_EXPERIMENTAL_TARGETS_TO_BUILD MATCHES "X86")
  list(APPEND LLVM_SNIPPY_TARGETS_LIST X86)
endif()

# lit discovers unit test executables by the "Tests" suffix (llvm/test/Unit/lit.cfg.py).
# A binary named otherwise is built but never run!
function(add_snippy_unittest test_name)
  if(NOT test_name MATCHES "Tests$")
    message(FATAL_ERROR
      "Unit test target '${test_name}' must end with 'Tests', "
      "otherwise llvm-lit will not run it")
  endif()
  add_llvm_target_unittest(${test_name} ${ARGN})
endfunction()

add_subdirectory(RandomUtils)
add_subdirectory(Target)

set(snippy_sources
  Support.cpp
  ImmediateHistogram.cpp
  MemoryAccessRange.cpp
  FormattedAPIntWithSign.cpp
  HistogramPatterns.cpp
  ProbabilityUtils.cpp
  YAMLTuple.cpp
  YAMLProbableItems.cpp
  )

set(snippy_link_libraries
  LLVMSnippy
  )

set(snippy_includes
  ${snippy_unittests_include}
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV/
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
  )

add_snippy_unittest(LLVMSnippyCommonTests
  ${snippy_sources}
  )

target_link_libraries(LLVMSnippyCommonTests PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyCommonTests PUBLIC ${snippy_includes})
set_property(TARGET LLVMSnippyCommonTests PROPERTY FOLDER "Tests/UnitTests/ToolTests")
