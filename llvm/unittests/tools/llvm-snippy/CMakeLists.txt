set(LLVM_LINK_COMPONENTS
  Support
  )

set(snippy_sources
  Support.cpp
  ImmediateHistogram.cpp
  )

set(snippy_link_libraries
  LLVMSnippySupport
  LLVMSnippyConfig
  )

set(snippy_includes)

add_llvm_target_unittest(LLVMSnippyTests
  ${snippy_sources}
)

target_link_libraries(LLVMSnippyTests PUBLIC ${snippy_link_libraries})
target_include_directories(LLVMSnippyTests PUBLIC ${snippy_includes})

set_property(TARGET LLVMSnippyTests PROPERTY FOLDER "Tests/UnitTests/ToolTests")
