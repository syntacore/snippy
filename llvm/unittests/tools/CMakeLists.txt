if(LLVM_TARGETS_TO_BUILD MATCHES "X86")
  add_subdirectory(
    llvm-cfi-verify
  )
endif()

add_subdirectory(
  llvm-exegesis
)
add_subdirectory(llvm-profdata)
add_subdirectory(llvm-profgen)
add_subdirectory(llvm-mca)

if(LLVM_BUILD_SNIPPY)
  add_subdirectory(
    llvm-snippy
  )
endif()
