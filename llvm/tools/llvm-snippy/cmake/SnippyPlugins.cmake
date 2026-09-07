set(MODEL_PLUGIN_ORIGIN libRISCVModel.so)

if (NOT EXISTS "${RISCVModelSpike_DIR}")
  message(FATAL_ERROR "RISCVModelSpike_DIR should be set to a valid directory")
endif()
set(SPIKE_MODEL_SRC ${RISCVModelSpike_DIR}/${MODEL_PLUGIN_ORIGIN})
if (NOT EXISTS "${SPIKE_MODEL_SRC}")
  message(FATAL_ERROR "RISCVModelSpike_DIR should point to a directory containing ${MODEL_PLUGIN_ORIGIN}}")
endif()
set(RVM_SPIKE_PLUGIN riscv-spike-plugin.so)

if (NOT EXISTS "${RISCVModelWhisper_DIR}")
  message(FATAL_ERROR "RISCVModelWhisper_DIR should be set to a valid directory")
endif()
set(WHISPER_MODEL_SRC ${RISCVModelWhisper_DIR}/${MODEL_PLUGIN_ORIGIN})
if (NOT EXISTS "${WHISPER_MODEL_SRC}")
  message(FATAL_ERROR "RISCVModelWhisper_DIR should point to a directory containing ${MODEL_PLUGIN_ORIGIN}}")
endif()
set(RVM_WHISPER_PLUGIN riscv-whisper-plugin.so)

set(SPIKE_LIB_PATH ${LLVM_TOOLS_BINARY_DIR}/${RVM_SPIKE_PLUGIN})
set(WHISPER_LIB_PATH ${LLVM_TOOLS_BINARY_DIR}/${RVM_WHISPER_PLUGIN})
add_custom_command(
  OUTPUT ${SPIKE_LIB_PATH}
  MAIN_DEPENDENCY ${SPIKE_MODEL_SRC}
  COMMAND ${CMAKE_COMMAND} -E copy ${SPIKE_MODEL_SRC} ${SPIKE_LIB_PATH}
)

add_custom_command(
  OUTPUT ${WHISPER_LIB_PATH}
  MAIN_DEPENDENCY ${WHISPER_MODEL_SRC}
  COMMAND ${CMAKE_COMMAND} -E copy ${WHISPER_MODEL_SRC} ${WHISPER_LIB_PATH}
)
add_custom_target(llvm-snippy-plugins DEPENDS
  ${SPIKE_LIB_PATH}
  ${WHISPER_LIB_PATH}
)
