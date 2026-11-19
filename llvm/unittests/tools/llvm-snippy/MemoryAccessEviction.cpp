//===-- MemoryAccessEviction.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for the eviction memory scheme.
///
//===----------------------------------------------------------------------===//

#include "snippy/Config/MemoryScheme.h"
#include "snippy/unittests/GMockHelpers.h"

#include "llvm/ADT/STLExtras.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;

struct MemoryEvictionTestCase {
  MemoryAccessEviction MAE;

  MemoryEvictionTestCase(MemAddr Mask, MemAddr Fixed,
                         std::optional<size_t> AccessSize = std::nullopt) {
    MAE.Mask = Mask;
    MAE.Fixed = Fixed;
    MAE.AccessSize = AccessSize;
  }

  MemoryAccessEviction getEviction() const { return MAE; }
};

class MemoryEvictionTest
    : public ::testing::TestWithParam<MemoryEvictionTestCase> {
protected:
  static constexpr size_t MaxSchemeAccessSize = 8;

  void SetUp() override {
    RandEngine::init(::testing::GTEST_FLAG(random_seed));
  }
};

MATCHER_P(IsSchemeAddress, MAE, "") {
  if ((arg ^ MAE.Fixed) & ~MAE.Mask)
    return false;
  return arg >= MAE.Fixed && arg <= MAE.Fixed + MAE.Mask;
}

MATCHER_P2(IsValidAddress, MAE, GenInfo, "") {
  using namespace ::testing;
  using llvm::snippy::testing::explainInnerMatchResult;
  const AddressInfo &AI = arg;
  bool Result = true;
  auto Alignment = GenInfo.getMinAlignment();
  Result &= explainInnerMatchResult(Eq(GenInfo.AccessSize), AI.AccessSize,
                                    "access size", result_listener);
  Result &= explainInnerMatchResult(Eq(0u), AI.Address % Alignment,
                                    "address alignment", result_listener);
  Result &= explainInnerMatchResult(IsSchemeAddress(MAE), AI.Address,
                                    "scheme address", result_listener);
  Result &= explainInnerMatchResult(Le(0), AI.MinOffset, "min offset",
                                    result_listener);
  Result &= explainInnerMatchResult(Ge(0), AI.MaxOffset, "max offset",
                                    result_listener);

  // And the mask and the alignment mask.
  auto EffMask = MAE.Mask & ~(Alignment - 1);
  if (!EffMask) {
    Result &= explainInnerMatchResult(Eq(0), AI.MinOffset, "min offset",
                                      result_listener);
    Result &= explainInnerMatchResult(Eq(0), AI.MaxOffset, "max offset",
                                      result_listener);
    Result &= explainInnerMatchResult(Eq(0u), AI.MinStride, "min stride",
                                      result_listener);
    return Result;
  }
  Result &= explainInnerMatchResult(Ne(0), AI.MinStride, "min stride",
                                    result_listener);
  Result &= explainInnerMatchResult(
      Eq(0u), static_cast<uint64_t>(AI.MinOffset) % AI.MinStride,
      "min offset alignment", result_listener);
  Result &= explainInnerMatchResult(
      Eq(0u), static_cast<uint64_t>(AI.MaxOffset) % AI.MinStride,
      "max offset alignment", result_listener);
  Result &=
      explainInnerMatchResult(IsSchemeAddress(MAE), AI.Address + AI.MinOffset,
                              "min scheme address", result_listener);
  Result &=
      explainInnerMatchResult(IsSchemeAddress(MAE), AI.Address + AI.MaxOffset,
                              "max scheme address", result_listener);
  return Result;
}

TEST_P(MemoryEvictionTest, IsLegalAccessSizeTest) {
  auto MAE = GetParam().getEviction();
  auto MaxAccessSize = MAE.AccessSize ? *MAE.AccessSize : MaxSchemeAccessSize;
  for (size_t AccessSize = 1; AccessSize <= 64; AccessSize *= 2) {
    for (bool AllowMisalign : {false, true}) {
      AddressGenInfo GenInfo(AccessSize, /* Alignment */ 1u, AllowMisalign,
                             /* BurstMode */ false);
      EXPECT_EQ(MAE.isLegal(GenInfo), AccessSize <= MaxAccessSize);
    }
  }
}

TEST_P(MemoryEvictionTest, IsLegalAlignmentTest) {
  auto MAE = GetParam().getEviction();
  for (size_t Alignment = 1; Alignment <= 32; Alignment *= 2) {
    AddressGenInfo Misaligned(1u, Alignment, /* AllowMisalign */ true,
                              /* BurstMode */ false);
    EXPECT_TRUE(MAE.isLegal(Misaligned));
    AddressGenInfo Aligned(1u, Alignment, /* AllowMisalign */ false,
                           /* BurstMode */ false);
    EXPECT_EQ(MAE.isLegal(Aligned), !((Alignment - 1) & MAE.Fixed));
  }
}

TEST_P(MemoryEvictionTest, IsLegalMultiElementTest) {
  auto MAE = GetParam().getEviction();
  // Eviction schemes do not support strided (multi-element) accesses.
  AddressGenInfo MultiAccess(1u, 1u, /* AllowMisalign */ true,
                             /* BurstMode */ false,
                             /* NumElements */ 2u, /* MinStride */ 1u);
  EXPECT_FALSE(MAE.isLegal(MultiAccess));
}

TEST_P(MemoryEvictionTest, GetRandomAddressTest) {
  static const std::vector<AddressGenInfo> InstrAccesses = {
      {1u, 1u, true, false},  {1u, 2u, true, false},   {1u, 4u, true, false},
      {2u, 1u, true, false},  {2u, 2u, true, false},   {4u, 4u, true, false},
      {8u, 8u, true, false},  {1u, 1u, false, false},  {1u, 2u, false, false},
      {1u, 4u, false, false}, {2u, 2u, false, false},  {4u, 4u, false, false},
      {8u, 8u, false, false}, {8u, 16u, false, false},
  };
  auto MAE = GetParam().getEviction();

  llvm::for_each(InstrAccesses, [&](const auto &GenInfo) {
    if (!MAE.isLegal(GenInfo))
      return;
    for (unsigned I = 0; I < 1024; ++I) {
      auto Res = MAE.randomAddress(GenInfo);
      EXPECT_THAT(Res, IsValidAddress(MAE, GenInfo));
    }
  });
}

TEST_P(MemoryEvictionTest, GetPossibleAddressesTest) {
  auto MAE = GetParam().getEviction();
  auto MB = MAE.getPossibleAddresses();
  ASSERT_EQ(MB.size(), 1u);
  auto Range = *MB.begin();
  EXPECT_EQ(Range.Start, MAE.Fixed);
  EXPECT_EQ(Range.End, MAE.Fixed + MAE.Mask + MaxSchemeAccessSize);
}

INSTANTIATE_TEST_SUITE_P(
    MemoryEvictions, MemoryEvictionTest,
    ::testing::Values(
        MemoryEvictionTestCase(/* Mask */ 0x0u, /* Fixed */ 0x0u),
        MemoryEvictionTestCase(/* Mask */ 0x0u, /* Fixed */ 0x100u),
        MemoryEvictionTestCase(/* Mask */ 0x1u, /* Fixed */ 0x0u),
        MemoryEvictionTestCase(/* Mask */ 0x3u, /* Fixed */ 0x1u),
        MemoryEvictionTestCase(/* Mask */ 0x87u, /* Fixed */ 0x30u),
        MemoryEvictionTestCase(/* Mask */ 0xffu, /* Fixed */ 0x100u),
        MemoryEvictionTestCase(/* Mask */ 0x100u, /* Fixed */ 0x4u),
        MemoryEvictionTestCase(/* Mask */ 0x3fffu, /* Fixed */ 0x2000u),
        MemoryEvictionTestCase(/* Mask */ 0xffu, /* Fixed */ 0x100u,
                               /* AccessSize */ 4u),
        MemoryEvictionTestCase(/* Mask */ 0x3fffu, /* Fixed */ 0x4000u,
                               /* AccessSize */ 16u)));