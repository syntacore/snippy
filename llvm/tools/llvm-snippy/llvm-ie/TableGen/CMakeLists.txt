set(LLVM_LINK_COMPONENTS
  Support
  )

add_tablegen(llvm-ie-tblgen LLVM_IE
  TableGen.cpp
  InstructionFeature.cpp 
  RISCVInstrEnumerator.cpp
)

set(LLVM_IE_TABLEGEN_EXE $<TARGET_FILE:llvm-ie-tblgen> PARENT_SCOPE)
set(LLVM_IE_TABLEGEN_TARGET llvm-ie-tblgen PARENT_SCOPE)
