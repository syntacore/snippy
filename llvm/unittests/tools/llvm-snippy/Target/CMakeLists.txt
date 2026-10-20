add_subdirectory(RISCV)
if("X86" IN_LIST LLVM_SNIPPY_TARGETS_LIST)
  add_subdirectory(X86)
endif()
if("AArch64" IN_LIST LLVM_SNIPPY_TARGETS_LIST)
  add_subdirectory(AArch64)
endif()

# Target-independent LLVMState create* tests, parameterized over every backend
# enabled in this build. Link all LLVMSnippy target backends so that the
# tested state can be created for any of them.
set(snippy_target_tests_sources
  ${snippy_init_gtest_env}
  LLVMState.cpp
  )

set(snippy_target_tests_link_libraries
  LLVMSnippy
  )
foreach(t ${LLVM_SNIPPY_TARGETS_LIST})
  list(APPEND snippy_target_tests_link_libraries "LLVMSnippy${t}")
endforeach()

set(snippy_target_tests_includes
  ${snippy_unittests_include}
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV/
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
  ${LLVM_MAIN_SRC_DIR}/lib/Target/X86/
  ${LLVM_BINARY_DIR}/lib/Target/X86
  ${LLVM_MAIN_SRC_DIR}/lib/Target/AArch64/
  ${LLVM_BINARY_DIR}/lib/Target/AArch64
  )

add_snippy_unittest(LLVMSnippyLLVMStateTests
  ${snippy_target_tests_sources}
  )

target_link_libraries(LLVMSnippyLLVMStateTests PUBLIC ${snippy_target_tests_link_libraries})
target_include_directories(LLVMSnippyLLVMStateTests PUBLIC ${snippy_target_tests_includes})
set_property(TARGET LLVMSnippyLLVMStateTests PROPERTY FOLDER "Tests/UnitTests/ToolTests")
