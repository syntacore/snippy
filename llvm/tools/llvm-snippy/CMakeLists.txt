set(LLVM_BUILD_SNIPPY FALSE CACHE BOOL "Build llvm-snippy")
if (NOT LLVM_BUILD_SNIPPY)
  message(STATUS "Disable llvm-snippy")
  return()
endif()
message(STATUS "Enable llvm-snippy")

function(get_snippy_version path version)
  set(snippy_version_json_file "${path}/version.json")
  cmake_path(NORMAL_PATH snippy_version_json_file)

  if(NOT EXISTS "${snippy_version_json_file}")
    message(
      STATUS
        "Can't read llvm-snippy version from ${snippy_version_json_file}, using default"
    )
    set(${version}
        "1.0.0"
        PARENT_SCOPE)
    return()
  endif()

  file(READ ${snippy_version_json_file} snippy_version_json_contents)
  string(JSON snippy_semver ERROR_VARIABLE snippy_semver_error GET ${snippy_version_json_contents} semver)

  if(snippy_semver STREQUAL "" OR snippy_semver STREQUAL "semver-NOTFOUND")
    set(snippy_semver "0.0.0-dev")
  endif()

  set(${version}
      ${snippy_semver}
      PARENT_SCOPE)
endfunction()

if(NOT DEFINED LLVM_SNIPPY_VERSION)
  get_snippy_version(${CMAKE_CURRENT_SOURCE_DIR} LLVM_SNIPPY_VERSION)
endif()

string(REGEX MATCH "^([0-9]+)\.([0-9]+)\.([0-9]+)"
             LLVM_SNIPPY_SEMVER_PARSE_MATCH ${LLVM_SNIPPY_VERSION})

if(LLVM_SNIPPY_SEMVER_PARSE_MATCH STREQUAL "")
  message(
    FATAL_ERROR
      "Invalid llvm-snippy version specified: ${LLVM_SNIPPY_VERSION}, expected a semantic version"
  )
endif()

set(LLVM_SNIPPY_VERSION_MAJOR ${CMAKE_MATCH_1})
set(LLVM_SNIPPY_VERSION_MINOR ${CMAKE_MATCH_2})
set(LLVM_SNIPPY_VERSION_PATCH ${CMAKE_MATCH_3})

message(STATUS "LLVM_SNIPPY_VERSION: ${LLVM_SNIPPY_VERSION}")
message(STATUS "LLVM_SNIPPY_VERSION_MAJOR: ${LLVM_SNIPPY_VERSION_MAJOR}")
message(STATUS "LLVM_SNIPPY_VERSION_MINOR: ${LLVM_SNIPPY_VERSION_MINOR}")
message(STATUS "LLVM_SNIPPY_VERSION_PATCH: ${LLVM_SNIPPY_VERSION_PATCH}")

set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Object
  Support
  Target
  AllTargetsCodeGens
  AllTargetsDescs
  AllTargetsInfos
  )

set(LLVMSnippyDeps intrinsics_gen)
add_llvm_tool(llvm-snippy
  DISABLE_LLVM_LINK_LLVM_DYLIB
  llvm-snippy.cpp

  DEPENDS
  ${LLVMSnippyDeps}
  )

add_subdirectory(include)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_subdirectory(lib)


target_include_directories(LLVMSnippyConfig PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_include_directories(LLVMSnippySupport PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_include_directories(LLVMSnippySimulator PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
if (NOT RISCVModelHeaders_DIR)
  target_include_directories(LLVMSnippySimulator PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Model> $<INSTALL_INTERFACE:include/Model>)
  message(STATUS "RISCVModelHeaders_DIR is not provided. Try to find model interface headers in ${CMAKE_CURRENT_SOURCE_DIR}/Model")
endif()

target_link_libraries(llvm-snippy PRIVATE
  SnippyHeaders
  LLVMSnippy
  LLVMSnippyConfig
  LLVMSnippySupport
  LLVMSnippyTarget
  LLVMSnippyGenerator
  LLVMSnippyOptionCategory
  )


set(LLVM_SNIPPY_TARGETS_LIST ${LLVM_SNIPPY_TARGETS})
separate_arguments(LLVM_SNIPPY_TARGETS_LIST)

set(LLVM_SNIPPY_TARGETS "")

foreach(t ${LLVM_SNIPPY_TARGETS_LIST})
  message("Adding snippy target ${t}")
  set(LLVM_SNIPPY_TARGETS "${LLVM_SNIPPY_TARGETS}SNIPPY_TARGET(${t})\n")
endforeach()

configure_file(
  ${LLVM_MAIN_INCLUDE_DIR}/../tools/llvm-snippy/lib/Target/SnippyTargets.def.in
  ${LLVM_INCLUDE_DIR}/llvm/Config/SnippyTargets.def
  )

foreach(proj ${LLVM_SNIPPY_TARGETS_LIST})
  set(tgt "LLVMSnippy${proj}")
  message("Linking snippy target ${tgt}")
  target_link_libraries(llvm-snippy PRIVATE ${tgt})
endforeach()


add_dependencies(llvm-snippy lld)


