set(LLVM_LINK_COMPONENTS
  Support
  )

set(snippy_unittests_include ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(snippy_init_gtest_env ${CMAKE_CURRENT_SOURCE_DIR}/src/InitSnippyGTestEnv.cpp)

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

add_llvm_target_unittest(LLVMSnippyTestsCommon
  ${snippy_sources}
  )

target_link_libraries(LLVMSnippyTestsCommon PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyTestsCommon PUBLIC ${snippy_includes})
set_property(TARGET LLVMSnippyTestsCommon PROPERTY FOLDER "Tests/UnitTests/ToolTests")
