set(LLVM_BUILD_SNIPPY FALSE CACHE BOOL "Build llvm-snippy")
if (NOT LLVM_BUILD_SNIPPY)
  message(STATUS "Disable llvm-snippy")
  return()
endif()
message(STATUS "Enable llvm-snippy")

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
  LLVMSnippy
  LLVMSnippyConfig
  LLVMSnippySupport
  LLVMSnippyTarget
  LLVMSnippyGenerator
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


