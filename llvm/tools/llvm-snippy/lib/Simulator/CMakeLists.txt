set(LLVMSnippySimulatorSources
    SelfcheckObserver.cpp
    Simulator.cpp
    RISCVSimulator.cpp
    X86Simulator.cpp
    Transactions.cpp
    RISCVTraceObserver.cpp
    RISCVConverterSNTF.cpp
    RISCVFeatureFlagsSNTF.cpp
    )

add_llvm_library(LLVMSnippySimulator
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  ${LLVMSnippySimulatorSources}
  )

target_link_libraries(LLVMSnippySimulator PRIVATE LLVMSnippyConfig LLVMSnippySupport)

if (LLVM_TARGETS_TO_BUILD MATCHES "RISCV")
  target_include_directories(
    LLVMSnippySimulator
    PRIVATE
    ${LLVM_MAIN_SRC_DIR}/lib/Target/RISCV
    ${LLVM_BINARY_DIR}/lib/Target/RISCV
  )
  find_package(PkgConfig)
  if (PkgConfig_FOUND)
    pkg_check_modules(rvmi IMPORTED_TARGET rvmi)
  endif()
  if (rvmi_FOUND)
    target_link_libraries(LLVMSnippySimulator PUBLIC PkgConfig::rvmi)
  else()
    add_subdirectory(../../Model/RISCVModel ${CMAKE_CURRENT_BINARY_DIR}/rvmi)
    target_link_libraries(LLVMSnippySimulator PUBLIC rvmi)
  endif()
endif()
