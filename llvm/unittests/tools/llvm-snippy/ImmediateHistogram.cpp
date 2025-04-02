#include "snippy/Config/ImmediateHistogram.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;
TEST(ImmediateHistogram, genImmInInterval) {
  RandEngine::init(1);
  auto Res = genImmInInterval<-1, 1>(StridedImmediate(0, 0, 1));
  EXPECT_EQ(Res, 0);
  Res = genImmInInterval<-4, 4>(StridedImmediate(-3, 3, 3));
  EXPECT_EQ(Res % 3, 0);
  EXPECT_GE(Res, -4);
  EXPECT_LE(Res, 4);
  Res = genImmInInterval<-5, 5>(StridedImmediate(-8, 8, 4));
  EXPECT_EQ(Res % 4, 0);
  EXPECT_GE(Res, -5);
  EXPECT_LE(Res, 5);
  Res = genImmInInterval<0, 8>(StridedImmediate(-4, 16, 4));
  EXPECT_EQ(Res % 4, 0);
  EXPECT_GE(Res, 0);
  EXPECT_LE(Res, 8);
  Res = genImmInInterval<-16, -8>(StridedImmediate(-12, 4, 4));
  EXPECT_EQ(Res % 4, 0);
  EXPECT_GE(Res, -16);
  EXPECT_LE(Res, -8);
  Res = genImmInInterval<1, 9>(StridedImmediate(-24, 16, 8));
  EXPECT_EQ(Res, 8);
  Res = genImmInInterval<-16, -8>(StridedImmediate(-12, 4, 4));
  EXPECT_EQ(Res % 4, 0);
  EXPECT_GE(Res, -16);
  EXPECT_LE(Res, -8);
  Res = genImmInInterval<-7, 7>(StridedImmediate(-13, 14, 5));
  EXPECT_EQ(Res % 5, -13 % 5);
  EXPECT_GE(Res, -7);
  EXPECT_LE(Res, 7);
  Res = genImmInInterval<3, 12>(StridedImmediate(-13, 16, 8));
  EXPECT_EQ(Res, 5);
}
