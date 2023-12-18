add_llvm_library(LLVMSnippySimulator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  SelfcheckObserver.cpp
  Simulator.cpp
  RISCVSimulator.cpp
  X86Simulator.cpp
  Transactions.cpp
  )

target_link_libraries(LLVMSnippySimulator PRIVATE LLVMSnippyConfig LLVMSnippySupport)

if (LLVM_TARGETS_TO_BUILD MATCHES "RISCV")
  target_include_directories(
    LLVMSnippySimulator
    PRIVATE
    ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV
    ${LLVM_BINARY_DIR}/lib/Target/RISCV
  )

  target_include_directories(LLVMSnippySimulator PUBLIC ${RISCVModelHeaders_DIR})
endif()
