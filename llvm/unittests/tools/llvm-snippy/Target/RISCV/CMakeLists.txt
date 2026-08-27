set(snippy_sources
  ${snippy_init_gtest_env}
  Target.cpp
  )

set(snippy_link_libraries
  LLVMSnippy
  LLVMSnippyRISCV
  LLVMSnippyAArch64
  )

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
