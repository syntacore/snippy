//===-- MemorySchemePluginCInterface.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef MEMORY_SCHEME_PLUGIN_C_INTERFACE_H_
#define MEMORY_SCHEME_PLUGIN_C_INTERFACE_H_

#include <stddef.h>

#define MEMORY_SCHEME_PLUGIN_ENTRY_NAME "SnippyMemSchemePluginFuncTable"

#ifdef __cplusplus
extern "C" {
#endif

// Process of random address generation:
//            Snippy pipeline            Plugin pipeline
//  MemoryScheme::randomAddress(...) {
//    if plugin enabled           --->  generateAddress(...)
//      if CAN_GENERATE_ADDRESS           returns address if it can,
//        returns AddressInfo with        otherwise sets GenMode
//        generated address,              to CANNOT_GENERATE_ADDRESS
//        given access size and
//        other values
//
//    if plugin enabled &&
//       CANNOT_GENERATE_ADDRESS  --->  generateAddressId(...)
//      generates valid address           sets address identifier to any value
//      based on address identifier
//  }

// if getRandomAddress() requests are the same
//  and all fields of the AddressGlobalId are the same in pairs,
//  then the same addresses will be generated.
// Each combination of AddressGlobalId fields will result in valid address

struct AddressId {
  size_t MainId = 0;
  size_t OffsetId = 0;
};

struct AddressGlobalId {
  size_t MemSchemeId = 0;
  AddressId AddrId;
};

typedef enum GenerationMode {
  CANNOT_GENERATE_ADDRESS = 0,
  CAN_GENERATE_ADDRESS = 1
} GenModeT;

size_t snippyPluginGenerateAddress(size_t AccessSize, size_t Alignment,
                                   bool BurstMode, size_t InstrClassId,
                                   GenModeT *GenMode);
void snippyPluginGenerateAddressId(struct AddressGlobalId *AddrId);
void snippyPluginSetAddressInfoFile(const char *AddressInfoFileName);

// table for linking with snippy
struct MemorySchemePluginFunctionsTable {
  size_t (*generateAddress)(size_t AccessSize, size_t Alignment, bool BurstMode,
                            size_t InstrClassId, GenModeT *GenMode);
  void (*generateAddressId)(struct AddressGlobalId *AddrId);
  void (*setAddressInfoFile)(const char *AddressInfoFileName);
};

#ifdef __cplusplus
}
#endif

#endif // MEMORY_SCHEME_PLUGIN_C_INTERFACE_H_