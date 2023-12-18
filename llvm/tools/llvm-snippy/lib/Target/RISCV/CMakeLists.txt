include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
)

set(LLVM_LINK_COMPONENTS
  RISCV
  Core
  Support
  )

add_llvm_library(LLVMSnippyRISCV
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  RVVUnitConfig.cpp
  Target.cpp
  TargetGenContext.cpp
  DEPENDS
  intrinsics_gen
  RISCVCommonTableGen
  )

# Why do we need LLVMSnippyConfig{Support/Target}?
target_link_libraries(LLVMSnippyRISCV PRIVATE
  LLVMSnippyConfig LLVMSnippySupport LLVMSnippyTarget)
