set(snippy_sources
  ${snippy_init_gtest_env}
  Target.cpp
  )

set(snippy_link_libraries
  LLVMSnippy
  )
foreach(t ${LLVM_SNIPPY_TARGETS_LIST})
  set(tgt "LLVMSnippy${t}")
  message(STATUS "Linknig Target unittests to ${tgt}")
  list(APPEND snippy_link_libraries "${tgt}")
endforeach()


set(snippy_includes
  ${snippy_unittests_include}
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV/
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
  )

add_llvm_target_unittest(LLVMSnippyTestsTargetRISCV
  ${snippy_sources}
  )

target_link_libraries(LLVMSnippyTestsTargetRISCV PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyTestsTargetRISCV PUBLIC ${snippy_includes})
set_property(TARGET LLVMSnippyTestsTargetRISCV PROPERTY FOLDER "Tests/UnitTests/ToolTests")
