include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/X86
  ${LLVM_BINARY_DIR}/lib/Target/X86
)

set(LLVM_LINK_COMPONENTS
  X86
  Core
  Support
  )

add_llvm_library(LLVMSnippyX86
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  Target.cpp

  DEPENDS
  intrinsics_gen
  X86CommonTableGen
  )

target_link_libraries(LLVMSnippyX86 PRIVATE
  LLVMSnippyConfig LLVMSnippySupport LLVMSnippyTarget)
