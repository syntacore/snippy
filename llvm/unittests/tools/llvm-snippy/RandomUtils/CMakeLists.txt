set(snippy_sources
  Random.cpp
  )

set(snippy_link_libraries
  LLVMSnippy
  )

set(snippy_includes
  ${snippy_unittests_include}
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV/
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
  )

add_snippy_unittest(LLVMSnippyRandomTests
  ${snippy_sources}
  )

target_link_libraries(LLVMSnippyRandomTests PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyRandomTests PUBLIC ${snippy_includes})
set_property(TARGET LLVMSnippyRandomTests PROPERTY FOLDER "Tests/UnitTests/ToolTests")
