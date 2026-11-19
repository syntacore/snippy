//===-- MemoryAccessAddresses.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for memory addresses.
///
//===----------------------------------------------------------------------===//

#include "snippy/Config/MemoryScheme.h"
#include "snippy/unittests/GMockHelpers.h"

#include "llvm/ADT/STLExtras.h"

#include "gmock/gmock-matchers.h"
#include "gtest/gtest.h"

using namespace llvm::snippy;

struct MemoryAddressesTestCase {
  MemoryAccessAddresses MAAdresses;

  MemoryAddressesTestCase(std::optional<size_t> NextAddressIdx,
                          AddressSeq Addresses,
                          std::optional<size_t> NextBurstIdx = std::nullopt,
                          std::vector<AddressInfo> Burst = {}) {
    MAAdresses.NextAddressIdx = NextAddressIdx;
    MAAdresses.Addresses = Addresses;
    MAAdresses.NextBurstIdx = NextBurstIdx;
    MAAdresses.Burst = Burst;
  }

  MemoryAccessAddresses getAddresses() const { return MAAdresses; }
};

class MemoryAddressesTest
    : public ::testing::TestWithParam<MemoryAddressesTestCase> {
protected:
  void SetUp() override {
    // Since we call MA.randomAddress() in each test, we need to
    // initialize the RandEngine to make each test deterministic.
    RandEngine::init(::testing::GTEST_FLAG(random_seed));
  }
};

MATCHER_P(IsLegalBurstEntryMinStride, Alignment, "") {
  return arg == 0 || std::max(Alignment, arg) % std::min(Alignment, arg) == 0;
}

MATCHER_P2(IsLegalBurstEntry, AccessSize, Alignment, "") {
  using namespace ::testing;
  using llvm::snippy::testing::explainInnerMatchResult;
  const AddressInfo &AI = arg;
  bool Result = true;
  Result &= explainInnerMatchResult(Ge(AccessSize), AI.AccessSize,
                                    "access size", result_listener);
  Result &= explainInnerMatchResult(Eq(0), AI.Address % Alignment,
                                    "address alignment", result_listener);
  Result &= explainInnerMatchResult(Ge(AccessSize), AI.MaxOffset, "max offset",
                                    result_listener);
  Result &=
      explainInnerMatchResult(IsLegalBurstEntryMinStride(Alignment),
                              AI.MinStride, "min stride", result_listener);
  return Result;
}

MATCHER_P2(IsValidBurstAddress, MA, GenInfo, "") {
  using namespace ::testing;
  using llvm::snippy::testing::explainInnerMatchResult;
  const AddressInfo &AI = arg;
  bool Result = true;
  auto It = llvm::find_if(MA.Burst, [&](const AddressInfo &AddrInfo) {
    return AddrInfo.Address == AI.Address;
  });
  Result &= explainInnerMatchResult(Eq(GenInfo.AccessSize), AI.AccessSize,
                                    "access size", result_listener);
  Result &= explainInnerMatchResult(Ne(MA.Burst.end()), It,
                                    "burst address iterator", result_listener);
  if (!Result)
    return Result;
  Result &= explainInnerMatchResult(
      IsLegalBurstEntry(GenInfo.AccessSize, GenInfo.getMinAlignment()), *It,
      "burst entry legality", result_listener);
  Result &= explainInnerMatchResult(
      Eq(static_cast<int64_t>(It->MaxOffset - GenInfo.AccessSize)),
      AI.MaxOffset, "max offset", result_listener);
  Result &= explainInnerMatchResult(Eq(It->MinOffset), AI.MinOffset,
                                    "min offset", result_listener);
  Result &= explainInnerMatchResult(Eq(It->MinStride), AI.MinStride,
                                    "min stride", result_listener);
  Result &=
      explainInnerMatchResult(Eq(0u), AI.Address % GenInfo.getMinAlignment(),
                              "address alignment", result_listener);
  return Result;
}

TEST_P(MemoryAddressesTest, IsLegalTest) {
  auto MA = GetParam().getAddresses();
  static const std::vector<AddressGenInfo> InstrAccesses = {
      {1u, 1u, true, false},  {1u, 2u, true, false},  {1u, 2u, false, false},
      {2u, 2u, false, false}, {4u, 2u, false, false}, {8u, 2u, false, false},
      {4u, 1u, true, false},  {8u, 4u, false, false},
  };
  for (const auto &G : InstrAccesses) {
    auto Alignment = G.getMinAlignment();
    bool Expected;
    if (MA.NextAddressIdx) {
      EXPECT_LT(*MA.NextAddressIdx, MA.Addresses.size());
      Expected = MA.Addresses[*MA.NextAddressIdx].Addr % Alignment == 0;
    } else {
      Expected = llvm::any_of(MA.Addresses, [&](const AccessAddress &AA) {
        return AA.Addr % Alignment == 0 && AA.AccessSize >= G.AccessSize;
      });
    }
    EXPECT_EQ(MA.isLegal(G), Expected);
  }
  // Burst mode: legal if at least one burst entry is legal.
  for (const auto &G : InstrAccesses) {
    AddressGenInfo Burst(G.AccessSize, G.Alignment, G.AllowMisalign, true);
    auto Expected = llvm::any_of(MA.Burst, [&](const AddressInfo &AI) {
      return ::testing::Matches(
          IsLegalBurstEntry(Burst.AccessSize, Burst.getMinAlignment()))(AI);
    });
    EXPECT_EQ(MA.isLegal(Burst), Expected);
  }

  EXPECT_FALSE(MA.isLegal(AddressGenInfo(1u, 1u, true, false, 2u, 1u)));
  EXPECT_FALSE(MA.isLegal(AddressGenInfo(1u, 1u, true, true, 2u, 1u)));
}

MATCHER_P2(IsValidPlainAddress, MA, GenInfo, "") {
  using namespace ::testing;
  using llvm::snippy::testing::explainInnerMatchResult;
  const AddressInfo &AI = arg;
  bool Result = true;
  Result &= explainInnerMatchResult(Eq(GenInfo.AccessSize), AI.AccessSize,
                                    "access size", result_listener);
  Result &= explainInnerMatchResult(Eq(0), AI.MinOffset, "min offset",
                                    result_listener);
  Result &= explainInnerMatchResult(Eq(0), AI.MaxOffset, "max offset",
                                    result_listener);
  Result &= explainInnerMatchResult(Eq(0u), AI.MinStride, "min stride",
                                    result_listener);
  auto It = llvm::find_if(MA.Addresses, [&](const AccessAddress &AA) {
    return AA.Addr == AI.Address;
  });
  Result &= explainInnerMatchResult(Ne(MA.Addresses.end()), It,
                                    "address iterator", result_listener);
  if (!Result)
    return Result;
  Result &=
      explainInnerMatchResult(Eq(0u), AI.Address % GenInfo.getMinAlignment(),
                              "address alignment", result_listener);
  Result &=
      explainInnerMatchResult(Ge(GenInfo.AccessSize), It->AccessSize,
                              "address iterator access size", result_listener);
  return Result;
}

TEST_P(MemoryAddressesTest, GetRandomPlainAddressTest) {
  auto MA = GetParam().getAddresses();
  if (MA.Addresses.empty() || MA.NextAddressIdx)
    // The scheme is not in random plain mode
    GTEST_SKIP();
  static const std::vector<AddressGenInfo> InstrAccesses = {
      {1u, 1u, true, false}, {2u, 1u, true, false},  {4u, 2u, true, false},
      {8u, 4u, true, false}, {8u, 8u, false, false}, {1u, 4u, false, false},
      {2u, 4u, true, false}, {4u, 4u, false, false},
  };
  llvm::for_each(InstrAccesses, [&](const auto &GenInfo) {
    if (!MA.isLegal(GenInfo))
      return;
    for (unsigned I = 0; I < 1024; ++I) {
      auto Res = MA.randomAddress(GenInfo);
      EXPECT_THAT(Res, IsValidPlainAddress(MA, GenInfo));
    }
  });
}

TEST_P(MemoryAddressesTest, OrderedPlainCycleTest) {
  auto MA = GetParam().getAddresses();
  if (!MA.NextAddressIdx) {
    // The scheme is not in ordered plain mode
    GTEST_SKIP();
  }
  auto InitialIdx = *MA.NextAddressIdx;
  auto NumAddresses = MA.Addresses.size();
  AddressGenInfo GenInfo(1u, 1u, true, false);
  ASSERT_TRUE(MA.isLegal(GenInfo));
  for (unsigned I = 0; I < NumAddresses * 3; ++I) {
    auto Res = MA.randomAddress(GenInfo);
    EXPECT_EQ(Res.Address, MA.Addresses[(InitialIdx + I) % NumAddresses].Addr);
    EXPECT_THAT(Res, IsValidPlainAddress(MA, GenInfo));
  }
}

TEST_P(MemoryAddressesTest, GetRandomBurstAddressTest) {
  auto MA = GetParam().getAddresses();
  if (MA.Burst.empty())
    // The scheme has no burst accesses
    GTEST_SKIP();
  static const std::vector<AddressGenInfo> InstrAccesses = {
      {1u, 1u, true, true},  {2u, 1u, true, true},  {4u, 1u, true, true},
      {2u, 2u, false, true}, {4u, 4u, false, true},
  };
  llvm::for_each(InstrAccesses, [&](const auto &GenInfo) {
    if (!MA.isLegal(GenInfo))
      return;
    for (unsigned I = 0; I < 1024; ++I) {
      auto Res = MA.randomAddress(GenInfo);
      EXPECT_THAT(Res, IsValidBurstAddress(MA, GenInfo));
    }
  });
}

TEST_P(MemoryAddressesTest, OrderedBurstCycleTest) {
  auto MA = GetParam().getAddresses();
  if (!MA.NextBurstIdx)
    // The scheme is not in ordered burst mode
    GTEST_SKIP();
  auto InitialIdx = *MA.NextBurstIdx;
  auto NumBursts = MA.Burst.size();
  AddressGenInfo GenInfo(1u, 1u, true, true);
  ASSERT_TRUE(MA.isLegal(GenInfo));
  for (unsigned I = 0; I < NumBursts * 3; ++I) {
    auto Res = MA.randomAddress(GenInfo);
    auto &Expected = MA.Burst[(InitialIdx + I) % NumBursts];
    EXPECT_EQ(Res.Address, Expected.Address);
    EXPECT_EQ(Res.MaxOffset, Expected.MaxOffset - 1);
    EXPECT_EQ(Res.AccessSize, 1u);
  }
}

MATCHER_P(isContainedInMB, MB, "") { return MB.contained(arg); }

TEST_P(MemoryAddressesTest, GetPossibleAddressesTest) {
  auto MA = GetParam().getAddresses();
  auto MB = MA.getPossibleAddresses();
  std::vector<AddressInfo> PlainAddresses;
  llvm::transform(MA.Addresses, std::back_inserter(PlainAddresses),
                  [&](auto &&Plain) {
                    return AddressInfo{Plain.Addr, 0, 0, Plain.AccessSize, 0u};
                  });
  EXPECT_THAT(PlainAddresses, ::testing::Each(isContainedInMB(MB)));
  EXPECT_THAT(MA.Burst, ::testing::Each(isContainedInMB(MB)));
}

INSTANTIATE_TEST_SUITE_P(
    MemoryAddresses, MemoryAddressesTest,
    ::testing::Values(
        // Ordered plain addresses.
        MemoryAddressesTestCase(
            /* NextAddressIdx */ 0u,
            AddressSeq{{0x100, 8u}, {0x110, 8u}, {0x120, 8u}}),
        // Random plain addresses with different access sizes.
        MemoryAddressesTestCase(
            std::nullopt, AddressSeq{{0x100, 8u}, {0x210, 4u}, {0x308, 8u}}),
        // Random plain addresses with a low unaligned entry.
        MemoryAddressesTestCase(
            std::nullopt, AddressSeq{{0x100, 8u}, {0x101, 4u}, {0x300, 8u}}),
        // Burst-only scheme.
        MemoryAddressesTestCase(std::nullopt, AddressSeq{},
                                /* NextBurstIdx */ std::nullopt,
                                std::vector<AddressInfo>{
                                    {0x1000, 64, 0, 8, 8}}),
        // Ordered burst-only scheme.
        MemoryAddressesTestCase(
            std::nullopt, AddressSeq{}, /* NextBurstIdx */ 0u,
            std::vector<AddressInfo>{{0x2000, 64, 0, 8, 8},
                                     {0x2040, 32, 0, 8, 8}}),
        // Both plain (ordered) and burst accesses.
        MemoryAddressesTestCase(
            /* NextAddressIdx */ 0u, AddressSeq{{0x100, 8u}, {0x200, 8u}},
            /* NextBurstIdx */ std::nullopt,
            std::vector<AddressInfo>{{0x3000, 64, 0, 8, 8}})));