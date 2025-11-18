#include "snippy/Config/Valuegram.h"
#include "llvm/ADT/APFloat.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using llvm::APFloat;
using namespace llvm::snippy;

namespace {

MATCHER_P(isAPFloatEqMatcher, APFloatExpected, "") {
  if (!arg)
    return false;

  auto &ExpectedSemantics = [&]() -> auto & {
    switch (arg->Format) {
    case InputFormat::FPHalf:
      return APFloat::IEEEhalf();
    case InputFormat::FPSingle:
      return APFloat::IEEEsingle();
    case InputFormat::FPDouble:
      return APFloat::IEEEdouble();
    default:
      llvm_unreachable("unexpected float semantics");
    }
  }();

  if (&ExpectedSemantics != &APFloatExpected.getSemantics())
    return false;

  if (arg->getVal() != APFloatExpected.bitcastToAPInt())
    return false;

  return true;
}

template <typename... Ts> auto isAPFloatEq(Ts &&...Args) {
  return isAPFloatEqMatcher(APFloat(std::forward<Ts>(Args)...));
}

inline auto isPosInfinity(const llvm::fltSemantics &Sem) {
  return isAPFloatEqMatcher(APFloat::getInf(Sem));
}

inline auto isNegInfinity(const llvm::fltSemantics &Sem) {
  return isAPFloatEqMatcher(APFloat::getInf(Sem, true));
}

inline auto isQNaN(const llvm::fltSemantics &Sem, bool Neg = false) {
  return isAPFloatEqMatcher(APFloat::getQNaN(Sem, Neg));
}

class FormattedAPIntWithSignTest : public ::testing::Test {
protected:
  std::optional<FormattedAPIntWithSign> fromString(llvm::StringRef Str) const {
    auto Res = FormattedAPIntWithSign::fromString(Str);
    if (!Res) {
      llvm::consumeError(Res.takeError());
      return std::nullopt;
    }
    return std::move(*Res);
  }
};

TEST_F(FormattedAPIntWithSignTest, FPFormat) {
  EXPECT_THAT(fromString("inf"), isPosInfinity(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("+inf"), isPosInfinity(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("-inf"), isNegInfinity(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("infd"), isPosInfinity(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("+infd"), isPosInfinity(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("-infd"), isNegInfinity(APFloat::IEEEdouble()));

  EXPECT_THAT(fromString("inff"), isPosInfinity(APFloat::IEEEsingle()));
  EXPECT_THAT(fromString("+inff"), isPosInfinity(APFloat::IEEEsingle()));
  EXPECT_THAT(fromString("-inff"), isNegInfinity(APFloat::IEEEsingle()));

  EXPECT_THAT(fromString("infh"), isPosInfinity(APFloat::IEEEhalf()));
  EXPECT_THAT(fromString("+infh"), isPosInfinity(APFloat::IEEEhalf()));
  EXPECT_THAT(fromString("-infh"), isNegInfinity(APFloat::IEEEhalf()));

  EXPECT_THAT(fromString("nan"), isQNaN(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("nand"), isQNaN(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("+nand"), isQNaN(APFloat::IEEEdouble()));
  EXPECT_THAT(fromString("-nan"), isQNaN(APFloat::IEEEdouble(), true));
  EXPECT_THAT(fromString("-nand"), isQNaN(APFloat::IEEEdouble(), true));
  EXPECT_THAT(fromString("-nand"), isQNaN(APFloat::IEEEdouble(), true));

  EXPECT_THAT(fromString("nanf"), isQNaN(APFloat::IEEEsingle()));
  EXPECT_THAT(fromString("+nanf"), isQNaN(APFloat::IEEEsingle()));
  EXPECT_THAT(fromString("-nanf"), isQNaN(APFloat::IEEEsingle(), true));
  EXPECT_THAT(fromString("-nanf"), isQNaN(APFloat::IEEEsingle(), true));

  EXPECT_THAT(fromString("nanh"), isQNaN(APFloat::IEEEhalf()));
  EXPECT_THAT(fromString("+nanh"), isQNaN(APFloat::IEEEhalf()));
  EXPECT_THAT(fromString("-nanh"), isQNaN(APFloat::IEEEhalf(), true));
  EXPECT_THAT(fromString("-nanh"), isQNaN(APFloat::IEEEhalf(), true));

  // Various invalid cases that should be rejected.
  EXPECT_THAT(fromString("--1.0"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("-+1.0"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("+-1.0"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("--inf"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("-+inf"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("-+nan"), ::testing::Eq(std::nullopt));
  EXPECT_THAT(fromString("-+nanf"), ::testing::Eq(std::nullopt));

  EXPECT_THAT(fromString("3.0"), isAPFloatEq(3.0));
  EXPECT_THAT(fromString("3.0f"), isAPFloatEq(3.0f));
  EXPECT_THAT(fromString("0x1.8p+1"), isAPFloatEq(3.0));
  EXPECT_THAT(fromString("0x1.8p+1d"), isAPFloatEq(3.0));
  EXPECT_THAT(fromString("0x1.8p+1f"), isAPFloatEq(3.0f));
  EXPECT_THAT(fromString("0x1.8p+1h"),
              isAPFloatEq(APFloat::IEEEhalf(), "0x1.8p+1"));

  EXPECT_THAT(fromString("-0x1.1p-3"), isAPFloatEq(-17.0 / 128.0));
  EXPECT_THAT(fromString("-0x1.1p-3d"), isAPFloatEq(-17.0 / 128.0));
  EXPECT_THAT(fromString("-0x1.1p-3f"), isAPFloatEq(-17.0f / 128.0f));

  EXPECT_THAT(fromString("0.112233"), isAPFloatEq(0.112233));
  EXPECT_THAT(fromString("0.112233d"), isAPFloatEq(0.112233));
  EXPECT_THAT(fromString("0.112233f"), isAPFloatEq(0.112233f));

  EXPECT_THAT(fromString("-2.0e-2"), isAPFloatEq(-1.0 / 50.0));
  EXPECT_THAT(fromString("-2.0e-2d"), isAPFloatEq(-1.0 / 50.0));
  EXPECT_THAT(fromString("-2.0e-2f"), isAPFloatEq(-1.0f / 50.0f));
}

} // namespace
