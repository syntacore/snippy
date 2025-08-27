include(TableGen)
set(LLVM_LINK_COMPONENTS Support)

add_tablegen(snippy-tblgen SNIPPY
  RISCVGenerated.cpp
  SnippyOptions.cpp
  TableGen.cpp
  )

target_link_libraries(snippy-tblgen PRIVATE LLVMTableGenCommon)

# TODO: Fix cross-compilation. Because snippy is a part of the LLVM (cmake)
# project its tablegen depends on build_native_tool. That means that llvm-snippy
# tool has to either be enabled by default or should forward LLVM_BUILD_SNIPPY
# in llvm_create_cross_target. Borking cross-compilation is the least intrusive
# workaround.

# This should work but doesn't for the reasons explained above:

# set(SNIPPY_TABLEGEN_EXE "${SNIPPY_TABLEGEN_EXE}" CACHE INTERNAL "")
# set(SNIPPY_TABLEGEN_TARGET "${SNIPPY_TABLEGEN_TARGET}" CACHE INTERNAL "")

set(SNIPPY_TABLEGEN_EXE $<TARGET_FILE:snippy-tblgen> CACHE INTERNAL "")
set(SNIPPY_TABLEGEN_TARGET snippy-tblgen CACHE INTERNAL "")

