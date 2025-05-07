# Special common target for headers that propagates

# needed include directories and dependencies.

add_library(SnippyHeaders INTERFACE)

# Convenience variable for snippy-tblgen.
set(LLVM_SNIPPY_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)

target_include_directories(SnippyHeaders
  INTERFACE
  ${LLVM_SNIPPY_HEADERS_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  )

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snippy/Version/Version.inc.in
               ${CMAKE_CURRENT_BINARY_DIR}/snippy/Version/Version.inc)
