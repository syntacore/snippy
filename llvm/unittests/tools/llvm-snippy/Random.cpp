#include "snippy/Support/RandUtil.h"

#include "llvm/ADT/SmallVector.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;

template <typename T> struct SplitNIntoMPartsParams final {
  T N;
  T M;
  T Baseline = 0;
  double Uniformity = 0.0;
  T Alignment = 1;
};

class SplitNIntoMPartsTest
    : public testing::TestWithParam<SplitNIntoMPartsParams<unsigned>> {};

TEST_P(SplitNIntoMPartsTest, CheckInvariants) {
  auto &[N, M, Baseline, Uniformity, Alignment] = GetParam();

  for (unsigned seed = 0; seed < 1000; ++seed) {
    RandEngine::init(seed);
    llvm::SmallVector<unsigned> Parts;
    RandEngine::splitNIntoMParts<unsigned>(Parts, N, M, Baseline, Uniformity,
                                           Alignment);

    EXPECT_EQ(Parts.size(), M);

    auto sum = std::accumulate(Parts.begin(), Parts.end(), 0u);
    EXPECT_EQ(sum, N);

    for (const auto &P : Parts) {
      EXPECT_GE(P, 0u);
      EXPECT_GE(P, Baseline);
      EXPECT_GE(P, llvm::alignTo(Baseline, Alignment));
      EXPECT_EQ(P % Alignment, 0u);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    SplitNIntoMParts, SplitNIntoMPartsTest,
    testing::Values(
        // simplest cases
        SplitNIntoMPartsParams<unsigned>{0, 3},
        SplitNIntoMPartsParams<unsigned>{10, 1},
        SplitNIntoMPartsParams<unsigned>{10, 3},
        SplitNIntoMPartsParams<unsigned>{1, 10000},
        SplitNIntoMPartsParams<unsigned>{10000, 1},
        // With only baseline
        SplitNIntoMPartsParams<unsigned>{10, 3, 3},
        SplitNIntoMPartsParams<unsigned>{100, 100, 1},
        SplitNIntoMPartsParams<unsigned>{100, 10, 5},
        SplitNIntoMPartsParams<unsigned>{50, 10, 5},
        // With only uniformity
        SplitNIntoMPartsParams<unsigned>{10, 3, 0, 0.5},
        SplitNIntoMPartsParams<unsigned>{100, 10, 0, 1.0},
        SplitNIntoMPartsParams<unsigned>{50, 10, 0, 1.0},
        // With only alignment
        SplitNIntoMPartsParams<unsigned>{10, 3, 0, 0.0, 2},
        SplitNIntoMPartsParams<unsigned>{100, 10, 0, 0.0, 10},
        SplitNIntoMPartsParams<unsigned>{50, 10, 0, 0.0, 25},
        // Mixed
        SplitNIntoMPartsParams<unsigned>{10, 2, 3, 0.5, 2},
        SplitNIntoMPartsParams<unsigned>{100, 10, 5, 1.0, 10},
        SplitNIntoMPartsParams<unsigned>{50, 10, 0, 1.0, 25},
        // Hard and prime numbers
        SplitNIntoMPartsParams<unsigned>{28657, 1597, 17, 0.2341},
        SplitNIntoMPartsParams<unsigned>{4477457, 409, 53, 0.672},
        SplitNIntoMPartsParams<unsigned>{4477457, 409, 53, 1.0},
        SplitNIntoMPartsParams<unsigned>{204040, 30, 0, 0.0, 5101},
        SplitNIntoMPartsParams<unsigned>{909091, 101, 7675, 0.0}));

class SplitNIntoMPartsUniformityTest
    : public testing::TestWithParam<SplitNIntoMPartsParams<unsigned>> {};

TEST_P(SplitNIntoMPartsUniformityTest, UniformDistribution) {
  auto &[N, M, Baseline, Uniformity, Alignment] = GetParam();
  RandEngine::init(1);

  llvm::SmallVector<unsigned> Parts;
  RandEngine::splitNIntoMParts<unsigned>(Parts, N, M, Baseline, Uniformity,
                                         Alignment);

  auto MaxPart = *std::max_element(Parts.begin(), Parts.end());
  auto MinPart = *std::min_element(Parts.begin(), Parts.end());

  // When uniformity is 1.0, we first fill all parts evenly
  // as much as possible and then distribute the rest.
  EXPECT_LT(MaxPart - MinPart, Alignment * M);
}

INSTANTIATE_TEST_SUITE_P(
    SplitNIntoMParts, SplitNIntoMPartsUniformityTest,
    testing::Values(SplitNIntoMPartsParams<unsigned>{10, 3, 0, 1.0},
                    SplitNIntoMPartsParams<unsigned>{1, 10000, 0, 1.0},
                    SplitNIntoMPartsParams<unsigned>{10000, 1, 0, 1.0},
                    // with baseline
                    SplitNIntoMPartsParams<unsigned>{100, 100, 1, 1.0},
                    SplitNIntoMPartsParams<unsigned>{10, 3, 3, 1.0},
                    SplitNIntoMPartsParams<unsigned>{100, 10, 5, 1.0},
                    SplitNIntoMPartsParams<unsigned>{50, 10, 5, 1.0},
                    // with alignment
                    SplitNIntoMPartsParams<unsigned>{10, 3, 0, 1.0, 2},
                    SplitNIntoMPartsParams<unsigned>{100, 10, 0, 1.0, 10},
                    SplitNIntoMPartsParams<unsigned>{50, 10, 0, 1.0, 25}));

// There are cases where the result of splitNIntoMParts must be the same
// no matter the seed or internal implementation.
template <typename T> struct SplitNIntoMPartsParamsAndResult final {
  SplitNIntoMPartsParams<T> Params;
  llvm::SmallVector<T> Result;
};

class SplitNIntoMPartsDeterministicTest
    : public testing::TestWithParam<SplitNIntoMPartsParamsAndResult<unsigned>> {
};

TEST_P(SplitNIntoMPartsDeterministicTest, DeterministicTest) {
  auto &[Params, Expected] = GetParam();

  for (unsigned seed = 0; seed < 1000; ++seed) {
    RandEngine::init(seed);

    llvm::SmallVector<unsigned> Parts;
    RandEngine::splitNIntoMParts<unsigned>(Parts, Params.N, Params.M,
                                           Params.Baseline, Params.Uniformity,
                                           Params.Alignment);

    EXPECT_EQ(Parts, Expected);
  }
}

INSTANTIATE_TEST_SUITE_P(
    SplitNIntoMParts, SplitNIntoMPartsDeterministicTest,
    testing::Values(
        SplitNIntoMPartsParamsAndResult<unsigned>{{0, 3}, /*-->*/ {0, 0, 0}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{10, 1}, /*-->*/ {10}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{2, 2, 1}, /*-->*/ {1, 1}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{20, 4, 5},
                                                  /*-->*/ {5, 5, 5, 5}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{20, 4, 0, 1.0},
                                                  /*-->*/ {5, 5, 5, 5}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{20, 2, 0, 1.0},
                                                  /*-->*/ {10, 10}},
        SplitNIntoMPartsParamsAndResult<unsigned>{{20, 2, 1, 0.0, 10},
                                                  /*-->*/ {10, 10}},
        SplitNIntoMPartsParamsAndResult<unsigned>{
            {562448658, 2, 0, 1.0},
            /*-->*/ {281224329, 281224329}},
        SplitNIntoMPartsParamsAndResult<unsigned>{
            {std::numeric_limits<unsigned>::max(), 1},
            /*-->*/ {std::numeric_limits<unsigned>::max()}}));

template <typename T> struct genNUniqInIntervalParams final {
  T Min;
  T Max;
  size_t N;
  std::function<bool(T)> FilterOut = [](T) { return false; };
};

class GenNUniqInIntervalTest
    : public testing::TestWithParam<genNUniqInIntervalParams<int>> {};

template <typename Container> bool sortAndCheckForDuplicates(Container &C) {
  std::sort(C.begin(), C.end());
  return std::adjacent_find(C.begin(), C.end()) != C.end();
}

TEST_P(GenNUniqInIntervalTest, CheckInvariats) {
  auto &[Min, Max, N, FilterOut] = GetParam();

  for (unsigned seed = 0; seed < 1000; ++seed) {
    RandEngine::init(seed);

    auto ResultExp =
        RandEngine::genNUniqInInterval<int>(Min, Max, N, FilterOut);
    ASSERT_TRUE(static_cast<bool>(ResultExp));

    auto &Result = *ResultExp;
    EXPECT_EQ(Result.size(), N);

    for (const auto &Val : Result) {
      EXPECT_GE(Val, Min);
      EXPECT_LE(Val, Max);
      EXPECT_FALSE(FilterOut(Val));
    }

    EXPECT_FALSE(sortAndCheckForDuplicates(Result));
  }
}

INSTANTIATE_TEST_SUITE_P(
    GenNUniqInInterval, GenNUniqInIntervalTest,
    testing::Values(
        // Simple
        genNUniqInIntervalParams<int>{0, 10, 2},
        // Zero values
        genNUniqInIntervalParams<int>{0, 10, 0},
        // All values
        genNUniqInIntervalParams<int>{0, 10, 11},
        // Filter out one value
        genNUniqInIntervalParams<int>{0, 10, 1, [](int V) { return V == 5; }},
        // Filter out even
        genNUniqInIntervalParams<int>{0, 10, 3, [](int V) { return V % 2; }},
        // Filter out all but one
        genNUniqInIntervalParams<int>{0, 50, 1, [](int V) { return V != 25; }},
        // Small range
        genNUniqInIntervalParams<int>{5, 5, 1},
        // Negative
        genNUniqInIntervalParams<int>{-100, -90, 2}));
