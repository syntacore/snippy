set(LLVM_TARGET_DEFINITIONS ${PROJECT_SOURCE_DIR}/lib/Target/RISCV/RISCV.td)
tablegen(LLVM_IE RISCVInstrEnums.inc --arch=riscv -I ${PROJECT_SOURCE_DIR}/lib/Target/RISCV/)
add_public_tablegen_target(RISCVInstrEnumsTableGen)

add_llvm_library(LLVMInstructionEnumeratorRISCV
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  RISCVInstructionEnumerator.cpp

  DEPENDS
  RISCVInstrEnumsTableGen
)
