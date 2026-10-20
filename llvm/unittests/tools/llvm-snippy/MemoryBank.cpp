//===-- MemoryBank.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for memory bank.
///
//===----------------------------------------------------------------------===//

#include "snippy/Config/MemoryScheme.h"

#include "gtest/gtest.h"

namespace llvm::snippy {

// ================  Memory Range =====================

struct MemRangeInfo {
  MemAddr Start, End;
  size_t Size;
  bool ToBool;
};

struct MemoryRangeState : public ::testing::TestWithParam<MemRangeInfo> {
  MemRangeInfo State;
  MemoryRangeState() : State(GetParam()) {}
};

TEST_P(MemoryRangeState, MemRange) {
  MemRange MR(State.Start, State.End);

  EXPECT_EQ(MR.Start, State.Start);
  EXPECT_EQ(MR.End, State.End);

  EXPECT_EQ(MR.size(), State.Size);
  EXPECT_EQ(static_cast<bool>(MR), State.ToBool);

  EXPECT_EQ(MR, MR);
  EXPECT_FALSE(MR < MR);

  EXPECT_TRUE(MR.interfere(MR) || (MR.size() == 0));
  EXPECT_TRUE((MR.intersect(MR) == MR) || (MR.size() == 0));
}

INSTANTIATE_TEST_SUITE_P(
    MemRange, MemoryRangeState,
    ::testing::Values(
        // Empty range [0, 0)
        MemRangeInfo{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},

        // Smallest non-empty range [0, 1)
        MemRangeInfo{/*Start=*/0, /*End=*/1, /*Size=*/1, /*ToBool=*/true},

        // Unit range starting at 1: [1, 2)
        MemRangeInfo{/*Start=*/1, /*End=*/2, /*Size=*/1, /*ToBool=*/true},

        // Zero-length range not at origin: [5, 5)
        MemRangeInfo{/*Start=*/5, /*End=*/5, /*Size=*/0, /*ToBool=*/true},

        // Ordinary range [10, 20)
        MemRangeInfo{/*Start=*/10, /*End=*/20, /*Size=*/10, /*ToBool=*/true},

        // Large range [0, 1000000)
        MemRangeInfo{/*Start=*/0, /*End=*/1000000, /*Size=*/1000000,
                     /*ToBool=*/true},

        // Large addresses [0xFFFFFFFF00000000, 0xFFFFFFFF00000100)
        MemRangeInfo{/*Start=*/0xFFFFFFFF00000000ULL,
                     /*End=*/0xFFFFFFFF00000100ULL, /*Size=*/0x100,
                     /*ToBool=*/true},

        // Unit range near the top of the address space
        MemRangeInfo{/*Start=*/0xFFFFFFFFFFFFFFFFULL - 1,
                     /*End=*/0xFFFFFFFFFFFFFFFFULL, /*Size=*/1,
                     /*ToBool=*/true}));

struct TwoMemRangesInfo {
  MemRangeInfo First, Second;
  MemRange Intersection;
  bool Interfere, FirstGreater, Equal;
};

struct RelativeStateOfMemoryRanges
    : public ::testing::TestWithParam<TwoMemRangesInfo> {
  TwoMemRangesInfo State;
  RelativeStateOfMemoryRanges() : State(GetParam()) {}
};

TEST_P(RelativeStateOfMemoryRanges, MemRanges) {
  MemRange First(State.First.Start, State.First.End);
  MemRange Second(State.Second.Start, State.Second.End);

  EXPECT_EQ(First.interfere(Second), State.Interfere);
  EXPECT_EQ(Second.interfere(First), State.Interfere);
  EXPECT_EQ(First.intersect(Second), State.Intersection);
  EXPECT_EQ(Second.intersect(First), State.Intersection);
  EXPECT_EQ(First < Second, !State.FirstGreater && !State.Equal);
  EXPECT_EQ(Second < First, State.FirstGreater);
  EXPECT_EQ(First == Second, State.Equal);
  EXPECT_EQ(Second == First, State.Equal);
}

INSTANTIATE_TEST_SUITE_P(
    MemRange, RelativeStateOfMemoryRanges,
    ::testing::Values(
        // Identical ranges: [0, 1) == [0, 1)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/1, /*Size=*/1, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/1, /*Size=*/1, /*ToBool=*/true},
            /*Intersection=*/MemRange(0, 1),
            /*Interfere=*/true,
            /*FirstGreater=*/false,
            /*Equal=*/true},

        // Disjoint, First before Second: [0, 1) vs [2, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/1, /*Size=*/1, /*ToBool=*/true},
            /*Second=*/{/*Start=*/2, /*End=*/3, /*Size=*/1, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Disjoint, First after Second: [5, 7) vs [1, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/5, /*End=*/7, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/1, /*End=*/3, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Adjacent: [0, 2) vs [2, 4)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/2, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/2, /*End=*/4, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Adjacent reversed: [2, 4) vs [0, 2)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/2, /*End=*/4, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/2, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // First inside Second: [1, 3) vs [0, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/1, /*End=*/3, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Intersection=*/MemRange(1, 3),
            /*Interfere=*/true,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Second inside First: [0, 5) vs [1, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Second=*/{/*Start=*/1, /*End=*/3, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(1, 3),
            /*Interfere=*/true,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Partial overlap: [0, 3) vs [2, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/3, /*Size=*/3, /*ToBool=*/true},
            /*Second=*/{/*Start=*/2, /*End=*/5, /*Size=*/3, /*ToBool=*/true},
            /*Intersection=*/MemRange(2, 3),
            /*Interfere=*/true,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Partial overlap reversed: [2, 5) vs [0, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/2, /*End=*/5, /*Size=*/3, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/3, /*Size=*/3, /*ToBool=*/true},
            /*Intersection=*/MemRange(2, 3),
            /*Interfere=*/true,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Same start, different end: [0, 2) vs [0, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/2, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Intersection=*/MemRange(0, 2),
            /*Interfere=*/true,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Same end, different start: [0, 5) vs [2, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Second=*/{/*Start=*/2, /*End=*/5, /*Size=*/3, /*ToBool=*/true},
            /*Intersection=*/MemRange(2, 5),
            /*Interfere=*/true,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Zero-length at same point: [3, 3) vs [3, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Second=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/true},

        // Zero-length vs non-zero: [3, 3) vs [3, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Second=*/{/*Start=*/3, /*End=*/5, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Non-zero vs zero-length: [3, 5) vs [3, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/3, /*End=*/5, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Large addresses, different sizes
        TwoMemRangesInfo{/*First=*/{/*Start=*/0xFFFFFFFF00000000ULL,
                                    /*End=*/0xFFFFFFFF00000100ULL,
                                    /*Size=*/0x100, /*ToBool=*/true},
                         /*Second=*/
                         {/*Start=*/0xFFFFFFFF00000080ULL,
                          /*End=*/0xFFFFFFFF00000200ULL,
                          /*Size=*/0x180, /*ToBool=*/true},
                         /*Intersection=*/
                         MemRange(0xFFFFFFFF00000080ULL, 0xFFFFFFFF00000100ULL),
                         /*Interfere=*/true,
                         /*FirstGreater=*/false,
                         /*Equal=*/false},
        // Empty vs empty: [0, 0) vs [0, 0)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/true},

        // Empty vs non-empty: [0, 0) vs [0, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Non-empty vs empty: [0, 5) vs [0, 0)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/5, /*Size=*/5, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Empty vs non-empty, disjoint by start: [0, 0) vs [7, 9)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/7, /*End=*/9, /*Size=*/2, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Non-empty vs empty, disjoint by start: [7, 9) vs [0, 0)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/7, /*End=*/9, /*Size=*/2, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Empty inside non-empty range: [0, 0) vs [0, 10)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/0, /*End=*/10, /*Size=*/10, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Non-empty vs empty inside: [0, 10) vs [0, 0)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/10, /*Size=*/10, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Empty at same start as zero-length: [3, 3) vs [3, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Zero-length at different point: [3, 3) vs [5, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Second=*/{/*Start=*/5, /*End=*/5, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Zero-length reversed at different point: [5, 5) vs [3, 3)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/5, /*End=*/5, /*Size=*/0, /*ToBool=*/true},
            /*Second=*/{/*Start=*/3, /*End=*/3, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false},

        // Empty inside zero-length range: [0, 0) vs [5, 5)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Second=*/{/*Start=*/5, /*End=*/5, /*Size=*/0, /*ToBool=*/true},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/false,
            /*Equal=*/false},

        // Zero-length inside empty: [5, 5) vs [0, 0)
        TwoMemRangesInfo{
            /*First=*/{/*Start=*/5, /*End=*/5, /*Size=*/0, /*ToBool=*/true},
            /*Second=*/{/*Start=*/0, /*End=*/0, /*Size=*/0, /*ToBool=*/false},
            /*Intersection=*/MemRange(),
            /*Interfere=*/false,
            /*FirstGreater=*/true,
            /*Equal=*/false}));

TEST(MemRange, CtorStrangeValues) {
  MemRange T1;
  EXPECT_EQ(T1.Start, MemAddr(0));
  EXPECT_EQ(T1.End, MemAddr(0));

  MemRange T2(/*Start=*/MemAddr(0));
  EXPECT_EQ(T2.Start, MemAddr(0));
  EXPECT_EQ(T2.End, MemAddr(0));

  EXPECT_EQ(T1, T2);

  ASSERT_DEATH(MemRange(1, 0), "Illegal Memory Range");
}

// ================  Memory Bank =================

#define INITIALIZE_MEMORY_BANK(MB, ...)                                        \
  MemoryBank MB;                                                               \
  for (auto &&MR : {__VA_ARGS__})                                              \
  MB.addRange(std::move(MR))

TEST(MemoryBank, AddRangeFullCover) {
  // Adding a range that fully covers an existing one should replace it
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20});
  MB.addRange(MemRange{0, 30});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 30));
}

TEST(MemoryBank, AddRangeSpansMultiple) {
  // Adding a range that spans multiple existing ranges should merge them all
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 5}, MemRange{10, 15},
                         MemRange{20, 25});
  MB.addRange(MemRange{3, 22});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 25));
}

TEST(MemoryBank, AddRangeContiguousLeft) {
  // New range ends exactly where existing starts; should merge
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20});
  MB.addRange(MemRange{0, 10});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 20));
}

TEST(MemoryBank, AddRangeContiguousRight) {
  // New range starts exactly where existing ends; should merge
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 10});
  MB.addRange(MemRange{10, 20});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 20));
}

TEST(MemoryBank, AddRangeContiguousBoth) {
  // New range touches both neighbours; should merge all three
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 10}, MemRange{20, 30});
  MB.addRange(MemRange{10, 20});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 30));
}

TEST(MemoryBank, LowerUpperRangeBoundaries) {
  // Bank: [10,20) [30,40)
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20}, MemRange{30, 40});

  // Range starting exactly at existing range's End: [20, 25)
  {
    MemRange R{20, 25};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    EXPECT_EQ(L, U);
    EXPECT_EQ(std::distance(L, U), 0);
  }

  // Range ending exactly at existing range's Start: [5, 10)
  {
    MemRange R{5, 10};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    EXPECT_EQ(L, U);
    EXPECT_EQ(std::distance(L, U), 0);
  }

  // Range touching only the boundary between two ranges: [20, 30)
  {
    MemRange R{20, 30};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    EXPECT_EQ(L, U);
    EXPECT_EQ(std::distance(L, U), 0);
  }

  // Range ending exactly at existing range's End: [15, 20)
  {
    MemRange R{15, 20};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    std::vector<MemRange> Found(L, U);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0], MemRange(10, 20));
  }

  // Range starting exactly at existing range's Start: [30, 35)
  {
    MemRange R{30, 35};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    std::vector<MemRange> Found(L, U);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0], MemRange(30, 40));
  }

  // Range entirely inside bank's range: [12, 18)
  {
    MemRange R{12, 18};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    std::vector<MemRange> Found(L, U);
    ASSERT_EQ(Found.size(), 1u);
    EXPECT_EQ(Found[0], MemRange(10, 20));
  }

  // Range in the gap: [25, 30)
  {
    MemRange R{25, 30};
    auto L = MB.lowerRange(R);
    auto U = MB.upperRange(R);
    EXPECT_EQ(L, U);
    EXPECT_EQ(std::distance(L, U), 0);
  }
}

TEST(MemoryBank, ContainedAddressInfo) {
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 100}, MemRange{200, 300});

  // Fully contained: Address=10, MaxOffset=5, AccessSize=4 -> [10, 19)
  EXPECT_TRUE(MB.contained(AddressInfo{/*Address=*/10,
                                       /*MaxOffset=*/5,
                                       /*AccessSize=*/4}));

  // Contained exactly at the boundary: Address=90, MaxOffset=5, AccessSize=5
  // -> [90, 100)
  EXPECT_TRUE(MB.contained(AddressInfo{/*Address=*/90,
                                       /*MaxOffset=*/5,
                                       /*AccessSize=*/5}));

  // Not contained (spans gap between ranges): Address=90, MaxOffset=200
  EXPECT_FALSE(MB.contained(AddressInfo{/*Address=*/90,
                                        /*MaxOffset=*/200,
                                        /*AccessSize=*/4}));

  // Not contained (fully outside): Address=1000, MaxOffset=0, AccessSize=1
  EXPECT_FALSE(MB.contained(AddressInfo{/*Address=*/1000,
                                        /*MaxOffset=*/0,
                                        /*AccessSize=*/1}));

  // Inside the second range: Address=250, MaxOffset=10, AccessSize=4
  EXPECT_TRUE(MB.contained(AddressInfo{/*Address=*/250,
                                       /*MaxOffset=*/10,
                                       /*AccessSize=*/4}));
}

TEST(MemoryBank, ContainedInDerivedFromDiffIntersect) {
  // containedIn(Rhs) == diff(intersect(Rhs)).empty()
  INITIALIZE_MEMORY_BANK(A, MemRange{10, 20}, MemRange{30, 40});
  INITIALIZE_MEMORY_BANK(B, MemRange{0, 100});

  EXPECT_TRUE(A.containedIn(B));
  EXPECT_TRUE(B.containedIn(B));
  EXPECT_FALSE(B.containedIn(A));

  // Partial containment
  INITIALIZE_MEMORY_BANK(C, MemRange{10, 20});
  EXPECT_TRUE(C.containedIn(A));
  EXPECT_TRUE(C.containedIn(B));
  EXPECT_FALSE(A.containedIn(C));

  // Disjoint banks
  INITIALIZE_MEMORY_BANK(D, MemRange{200, 300});
  EXPECT_FALSE(A.containedIn(D));
  EXPECT_FALSE(D.containedIn(A));
}
TEST(MemoryBank, AddRangeExactDuplicate) {
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20});
  MB.addRange(MemRange{10, 20});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(10, 20));
}

TEST(MemoryBank, AddRangeInsideExisting) {
  // New range fully inside an existing one: should be a no-op
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 30});
  MB.addRange(MemRange{10, 20});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(0, 30));
}

TEST(MemoryBank, SubstractDisjointLeft) {
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20});
  MB.substractRange(MemRange{0, 5});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(10, 20));
}

TEST(MemoryBank, SubstractDisjointRight) {
  INITIALIZE_MEMORY_BANK(MB, MemRange{10, 20});
  MB.substractRange(MemRange{30, 40});
  EXPECT_EQ(MB.size(), 1lu);
  EXPECT_EQ(*MB.begin(), MemRange(10, 20));
}

TEST(MemoryBank, SubstractExactBoundaries) {
  // Substract an entire middle range without touching neighbours
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 10}, MemRange{20, 30},
                         MemRange{40, 50});
  MB.substractRange(MemRange{20, 30});
  EXPECT_EQ(MB.size(), 2lu);
  auto It = MB.begin();
  EXPECT_EQ(*It++, MemRange(0, 10));
  EXPECT_EQ(*It++, MemRange(40, 50));
}

TEST(MemoryBank, SubstractAcrossMultipleWithMiddleErased) {
  // Substract spanning multiple ranges, fully erasing the middle one
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 10}, MemRange{20, 30},
                         MemRange{40, 50});
  MB.substractRange(MemRange{5, 45});
  EXPECT_EQ(MB.size(), 2lu);
  auto It = MB.begin();
  EXPECT_EQ(*It++, MemRange(0, 5));
  EXPECT_EQ(*It++, MemRange(45, 50));
}

TEST(MemoryBank, ContainedAddressInfoZeroAccessSize) {
  INITIALIZE_MEMORY_BANK(MB, MemRange{0, 100});

  // AccessSize = 0, MaxOffset = 10 -> [10, 20)
  EXPECT_TRUE(MB.contained(AddressInfo{/*Address=*/10,
                                       /*MaxOffset=*/10,
                                       /*AccessSize=*/0}));

  // AccessSize = 0, MaxOffset = 0 -> [50, 50) (zero-length)
  EXPECT_TRUE(MB.contained(AddressInfo{/*Address=*/50,
                                       /*MaxOffset=*/0,
                                       /*AccessSize=*/0}));
}

TEST(MemoryBank, ContainedEmptyBank) {
  MemoryBank Empty;
  EXPECT_FALSE(Empty.contained(MemRange{0, 0}));
  EXPECT_FALSE(Empty.contained(MemRange{0, 1}));
  EXPECT_FALSE(Empty.contained(MemRange{5, 10}));
}

} // namespace llvm::snippy
