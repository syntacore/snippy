set(snippy_sources
  ${snippy_init_gtest_env}
  LLVMState.cpp
  )

set(snippy_link_libraries
  LLVMSnippy
  )

foreach(t ${LLVM_SNIPPY_TARGETS_LIST})
  set(tgt "LLVMSnippy${t}")
  message(STATUS "Linking Target unittests to ${tgt}")
  list(APPEND snippy_link_libraries "${tgt}")
endforeach()

set(snippy_includes
  ${snippy_unittests_include}
  ${LLVM_MAIN_SRC_DIR}/lib/Target/X86/
  ${LLVM_BINARY_DIR}/lib/Target/X86
  )

add_snippy_unittest(LLVMSnippyTargetX86Tests
  ${snippy_sources}
  )

target_link_libraries(LLVMSnippyTargetX86Tests PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyTargetX86Tests PUBLIC ${snippy_includes})
set_property(TARGET LLVMSnippyTargetX86Tests PROPERTY FOLDER "Tests/UnitTests/ToolTests")