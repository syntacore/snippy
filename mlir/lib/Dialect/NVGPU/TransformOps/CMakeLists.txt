add_mlir_dialect_library(MLIRNVGPUTransformOps
  NVGPUTransformOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${MLIR_MAIN_INCLUDE_DIR}/mlir/Dialect/NVGPU/TransformOps

  DEPENDS
  MLIRNVGPUTransformOpsIncGen

  LINK_LIBS PUBLIC
  MLIRAffineDialect
  MLIRArithDialect
  MLIRIR
  MLIRLinalgDialect
  MLIRNVGPUDialect
  MLIRParser
  MLIRSideEffectInterfaces
  MLIRTransformDialect
  MLIRTransformDialectUtils
  MLIRVectorTransforms
  )
