#include "RISCVModel/RVM.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

const char *rvm_strerror(RVMErrorCode Err) {
  switch (Err) {
  case RVM_ERRC_INVALID_ARGUMENT:
    return "Invalid argument";
  case RVM_ERRC_INVALID_ADDRESS:
    return "Invalid address";
  case RVM_ERRC_INVALID_MEM_REGION:
    return "Invalid memory region (RVMMemoryRegion)";
  case RVM_ERRC_INCOMPATIBLE:
    return "Library version is incompatible";
  case RVM_ERRC_VALUE_OUT_OF_RANGE:
    return "Value is out of valid range";
  case RVM_ERRC_IDX_OUT_OF_RANGE:
    return "Index is out of valid range";
  case RVM_ERRC_SUCCESS:
    return "Success";
  default:
    return "Unknown";
  }
}

#ifdef __cplusplus
}
#endif // __cplusplus
