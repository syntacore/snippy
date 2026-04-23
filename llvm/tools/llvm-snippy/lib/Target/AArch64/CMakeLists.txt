include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/AArch64
  ${LLVM_BINARY_DIR}/lib/Target/AArch64
)

set(LLVM_TARGET_DEFINITIONS ${PROJECT_SOURCE_DIR}/lib/Target/AArch64/AArch64.td)
tablegen(SNIPPY AArch64Operands.inc -gen-aarch64-operands -I ${PROJECT_SOURCE_DIR}/lib/Target/AArch64/)
set(LLVM_TARGET_DEFINITIONS ${CMAKE_CURRENT_SOURCE_DIR}/AArch64Generated.td)
tablegen(SNIPPY AArch64Generated.inc -gen-aarch64 -I ${PROJECT_SOURCE_DIR}/lib/Target/AArch64/)
add_public_tablegen_target(SnippyAArch64TableGen)

set(LLVM_LINK_COMPONENTS
  AArch64
  CodeGenTypes
  Core
  Support
  )

add_llvm_library(LLVMSnippyAArch64
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  Target.cpp

  DEPENDS
  intrinsics_gen
  AArch64CommonTableGen
  SnippyAArch64TableGen
  )

target_link_libraries(LLVMSnippyAArch64 PRIVATE
LLVMSnippyConfig LLVMSnippySupport LLVMSnippyTarget)
