set(LLVM_INSTRUCTION_ENUMERATOR_TARGETS)
foreach(target RISCV)
  if (${target} IN_LIST LLVM_TARGETS_TO_BUILD)
    list(APPEND LLVM_INSTRUCTION_ENUMERATOR_TARGETS ${target})
  endif()
endforeach()

set(LLVM_INSTRUCTION_ENUMERATOR_TARGETS ${LLVM_INSTRUCTION_ENUMERATOR_TARGETS} PARENT_SCOPE)

set(LLVM_IE_ENUM_TARGETS "")
foreach(target ${LLVM_INSTRUCTION_ENUMERATOR_TARGETS})
  add_subdirectory(${target})
  set(LLVM_IE_ENUM_TARGETS "${LLVM_IE_ENUM_TARGETS}LLVM_IE_TARGET(${target}InstructionEnumerator)\n")
endforeach()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/Targets.def.in
  ${CMAKE_CURRENT_BINARY_DIR}/Targets.def
  )

set(LLVM_LINK_COMPONENTS
  Support
  )

add_llvm_library(LLVMInstructionEnumerator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  SystemInitializer.cpp
  FeatureFilter.cpp
  InstructionEnumerator.cpp
)
