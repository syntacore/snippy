//===--- FeatureFilter.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_IE_LIB_FEATURE_FILTER_H
#define LLVM_TOOLS_LLVM_IE_LIB_FEATURE_FILTER_H

#include <set>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace llvm_ie {

std::set<llvm::StringRef> filterByFeatures(
    const std::set<llvm::StringRef> &Instrs,
    const llvm::DenseMap<llvm::StringRef, std::set<llvm::StringRef>>
        &FeatureTable);

}

#endif
