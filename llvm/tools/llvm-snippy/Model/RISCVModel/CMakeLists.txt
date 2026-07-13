add_llvm_library(rvmi DISABLE_LLVM_LINK_LLVM_DYLIB STATIC RVM.cpp)
target_include_directories(rvmi PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
                                       $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
