# Special common target for headers that propagates

# needed include directories and dependencies.

add_library(SnippyHeaders INTERFACE)

target_include_directories(SnippyHeaders
  INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  )

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/snippy/Version/Version.inc.in
               ${CMAKE_CURRENT_BINARY_DIR}/snippy/Version/Version.inc)
