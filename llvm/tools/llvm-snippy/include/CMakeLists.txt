# Special common target for headers that propagates

# needed include directories and dependencies.

# FIXME: Empty.cpp file is added so that we can build a static library via
# add_llvm_library. We need to use add_llvm_library because SnippyHeaders has to
# belong to an export set in order to link with other snippy libs
add_llvm_library(SnippyHeaders
  DISABLE_LLVM_LINK_LLVM_DYLIB
  STATIC
  Empty.cpp
  )

# Convenience variable for snippy-tblgen.
set(LLVM_SNIPPY_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)

target_include_directories(SnippyHeaders
  INTERFACE
  $<BUILD_INTERFACE:${LLVM_SNIPPY_HEADERS_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  )

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snippy/Version/Version.inc.in
               ${CMAKE_CURRENT_BINARY_DIR}/snippy/Version/Version.inc)
