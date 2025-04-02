#include "snippy/Support/Utils.h"

#include "gtest/gtest.h"

#include <cmath>
#include <random>

using namespace llvm::snippy;

template <typename T> struct MatchRemainderParams final {
  T Orig;
  T Num;
  T Div;
  std::optional<T> Res;
};
class MatchRemainderManualTest
    : public testing::TestWithParam<MatchRemainderParams<int>> {};

TEST_P(MatchRemainderManualTest, CheckExact) {
  auto &Params = GetParam();
  auto Res = matchRemainder(Params.Orig, Params.Num, Params.Div);
  ASSERT_EQ(Res.has_value(), Params.Res.has_value());
  if (Res.has_value())
    EXPECT_EQ(*Res, *Params.Res);
}

INSTANTIATE_TEST_SUITE_P(SnippySupport, MatchRemainderManualTest,
                         testing::Values(
                             // POSITIVE VALUES
                             // 1 % 1 == 1 % 1
                             MatchRemainderParams<int>{1, 1, 1, 1},
                             // 3 % 3 == 3 % 3
                             MatchRemainderParams<int>{3, 4, 3, 3},
                             // 3 % 2 = 3 % 2
                             MatchRemainderParams<int>{3, 4, 2, 3},
                             // 5 % 2 == 3 % 2
                             MatchRemainderParams<int>{5, 4, 2, 3},
                             // 5 % 2 == 3 % 2
                             MatchRemainderParams<int>{5, 3, 2, 3},
                             // Rem > OrigRem (5 % 3 > 10 % 3)
                             // 10 % 3 == 4 % 3
                             MatchRemainderParams<int>{10, 5, 3, 4},
                             // Rem < OrigRem (10 % 3 < 5 % 3)
                             // 5 % 3 == 8 % 3
                             MatchRemainderParams<int>{5, 10, 3, 8},
                             // 4 % 4 == 0 % 4
                             MatchRemainderParams<int>{4, 2, 4, 0},
                             MatchRemainderParams<int>{
                                 std::numeric_limits<int>::max() - 2,
                                 std::numeric_limits<int>::max() - 5,
                                 std::numeric_limits<int>::max(), std::nullopt},

                             // NEGATIVE VALUES
                             // -1 % 1 == -1 % 1
                             MatchRemainderParams<int>{-1, -1, 1, -1},
                             // Rem < OrigRem (abs(-4 % 3) < abs(-5 % 3))
                             // abs(-5 % 3) == abs(-2 % 3)
                             MatchRemainderParams<int>{-5, -4, 3, -2},
                             // Rem > OrigRem (abs(-5 % 3) > abs(-4 % 3))
                             // abs(-4 % 3) == abs(-4 % 3)
                             MatchRemainderParams<int>{-4, -5, 3, -4},
                             // Rem > OrigRem (abs(-5 % 3) > abs(-10 % 3))
                             // abs(-10 % 3) == abs(-4 % 3)
                             MatchRemainderParams<int>{-10, -5, 3, -4}));

template <typename T> class MatchRemainderRandomTest : public testing::Test {};

using MatchRemainderTypes = testing::Types<char, unsigned char, int, unsigned,
                                           long long, unsigned long long>;
TYPED_TEST_SUITE(MatchRemainderRandomTest, MatchRemainderTypes);
TYPED_TEST(MatchRemainderRandomTest, RandomParams) {
  std::uniform_int_distribution<TypeParam> NumDist(
      0, std::numeric_limits<TypeParam>::max() - 64);
  std::uniform_int_distribution<TypeParam> DivDist(2, 64);
  std::mt19937 Eng(::testing::GTEST_FLAG(random_seed));
  for (auto Step = 0; Step < 1000; ++Step) {
    auto Orig = NumDist(Eng);
    auto Num = NumDist(Eng);
    auto Div = DivDist(Eng);
    auto RoundTo = Num + (Step % 2);
    bool RoundDown = RoundTo <= Num;
    auto Matching = matchRemainder<TypeParam>(Orig, Num, Div, Num + (Step % 2));
    if (RoundDown && Num < Div && Num < Orig % Div) {
      EXPECT_EQ(Matching, std::nullopt);
      continue;
    }
    assert(Matching.has_value());
    ASSERT_TRUE(Matching.has_value());
    assert(*Matching % Div == Orig % Div);
    EXPECT_EQ(*Matching % Div, Orig % Div);
    if (RoundDown)
      EXPECT_LE(*Matching, Num);
    else
      EXPECT_GE(*Matching, Num);
  }
  if constexpr (std::is_signed_v<TypeParam>) {
    std::uniform_int_distribution<TypeParam> NumDist(
        std::numeric_limits<TypeParam>::min() + 64, -1);
    std::uniform_int_distribution<TypeParam> DivDist(2, 64);
    std::mt19937 Eng(::testing::GTEST_FLAG(random_seed));
    for (auto Step = 0; Step < 1000; ++Step) {
      auto Orig = NumDist(Eng);
      auto Num = NumDist(Eng);
      auto Div = DivDist(Eng);
      auto RoundTo = Num + (Step % 2);
      bool RoundDown = RoundTo >= Num;
      auto Matching = matchRemainder<TypeParam>(Orig, Num, Div, RoundTo);
      if (RoundDown && std::abs(Num) < Div &&
          std::abs(Num) < std::abs(Orig) % Div) {
        EXPECT_EQ(Matching, std::nullopt);
        continue;
      }
      ASSERT_TRUE(Matching.has_value());
      EXPECT_EQ(*Matching % Div, Orig % Div);
      if (RoundDown)
        EXPECT_LE(std::abs(static_cast<long long>(*Matching)),
                  std::abs(static_cast<long long>(Num)));
      else
        EXPECT_GE(std::abs(static_cast<long long>(*Matching)),
                  std::abs(static_cast<long long>(Num)));
    }
  }
}
