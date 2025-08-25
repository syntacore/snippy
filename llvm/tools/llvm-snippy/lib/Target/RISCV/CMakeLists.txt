include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV
  ${LLVM_BINARY_DIR}/lib/Target/RISCV
)

set(LLVM_TARGET_DEFINITIONS ${PROJECT_SOURCE_DIR}/lib/Target/RISCV/RISCV.td)
tablegen(SNIPPY RISCVGenerated.inc -gen-riscv -I ${PROJECT_SOURCE_DIR}/lib/Target/RISCV/)
add_public_tablegen_target(SnippyRISCVTableGen)

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
  SnippyRISCVTableGen
  )

target_link_libraries(
  LLVMSnippyRISCV
  PRIVATE LLVMSnippyConfig LLVMSnippySupport LLVMSnippyTarget
  PUBLIC SnippyHeaders
  )

target_include_directories(LLVMSnippyRISCV
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
  )
