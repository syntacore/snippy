//===-- BurstGram.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the unit-tests for BurstGram.
///
//===----------------------------------------------------------------------===//

#include "snippy/Config/BurstGram.h"

#include "RISCVInstrInfo.h"
#include "snippy/unittests/SnippyState.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace llvm::snippy {
using namespace llvm::RISCV;

namespace testing {

static LLVMState getDummyState() {
  return getLLVMState("riscv64", "", "", "+m,+a,+f,+d,+c,+v,+zcmp");
}

static OpcodeHistogram getDummyHistogram() {
  return SmallVector<std::pair<unsigned, double>>{
      {ADDI, 10.0},   {LUI, 2.0},      {LD, 8.0},      {SW, 6.0},
      {LB, 2.0},      {SH, 3.0},       {FENCE, 1.0},   {AND, 5.0},
      {OR, 4.0},      {XOR, 3.0},      {MUL, 4.0},     {DIV, 2.0},
      {REM, 1.5},     {FADD_D, 3.0},   {FMUL_S, 2.5},  {FSD, 3.0},
      {FLW, 2.0},     {VADD_VV, 5.0},  {VMUL_VX, 3.0}, {VLE32_V, 4.0},
      {VSE64_V, 3.0}, {AMOADD_W, 2.0}, {LR_D, 1.0}};
}

static BurstGramData::OpcodeGroupsTy getDummyBurstGramDataGroupings() {
  return {{ADD, SUB, AND},
          {MUL, DIV, REM},
          {FADD_D, FMUL_S, FSD},
          {VADD_VV, VMUL_VX, VLE32_V},
          {LD, SW, AMOADD_W}};
}

static BurstGramData::OpcodeGroupsTy getDummyBurstGramDataBaseRegisterGroups() {
  return {{ADD, SUB, AND},
          {MUL, DIV, REM},
          {FADD_D, FMUL_S, FSD},
          {VADD_VV, VMUL_VX, VLE32_V},
          {LD, SW, AMOADD_W}};
}

static OpcodeCache getOpcodeCache(const LLVMState &State) {
  return OpcodeCache(State.getSnippyTarget(), State.getInstrInfo(),
                     State.getSubtargetInfo());
}

// if in getBurstGramData functions some field of BurstGramData is not seted
// explicitly it means is has a dummy value, but BurstMode has CustomMode value

static BurstGramData getBurstGramData(
    BurstMode Mode, std::optional<BurstGramData::OpcodeGroupsTy> &&Groupings,
    std::optional<BurstGramData::OpcodeGroupsTy> &&BaseRegisterGroups) {
  return BurstGramData{/*Mode=*/Mode, /*MinSize=*/13u, /*MaxSize=*/666u,
                       /*Groupings=*/std::move(Groupings),
                       /*BaseRegisterGroups=*/std::move(BaseRegisterGroups)};
}

static BurstGramData
getBurstGramData(BurstMode Mode,
                 std::optional<BurstGramData::OpcodeGroupsTy> OpcodesGroup) {
  return getBurstGramData(Mode, std::move(OpcodesGroup),
                          getDummyBurstGramDataBaseRegisterGroups());
}

static BurstGramData getBurstGramData(BurstMode Mode) {
  return getBurstGramData(Mode, getDummyBurstGramDataGroupings(),
                          getDummyBurstGramDataBaseRegisterGroups());
}

static BurstGramData getBurstGramData(
    std::optional<BurstGramData::OpcodeGroupsTy> BaseRegisterGroups) {
  return getBurstGramData(BurstMode::CustomBurst,
                          getDummyBurstGramDataGroupings(),
                          std::move(BaseRegisterGroups));
}

} // namespace testing

struct BurstGramDataBurstNonConvertibleModesInit
    : public ::testing::TestWithParam<BurstMode> {};

TEST_P(BurstGramDataBurstNonConvertibleModesInit, NonConvertibleModes) {
  BurstGramData BGD = testing::getBurstGramData(GetParam());

  BurstGramData OldBGD = BGD;

  BGD.convertToCustomMode(testing::getDummyHistogram(),
                          testing::getDummyState().getInstrInfo());

  EXPECT_EQ(BGD.Mode, OldBGD.Mode);
  EXPECT_EQ(BGD.MinSize, OldBGD.MinSize);
  EXPECT_EQ(BGD.MaxSize, OldBGD.MaxSize);
  EXPECT_EQ(BGD.Groupings, OldBGD.Groupings);
  EXPECT_EQ(BGD.BaseRegisterGroups, OldBGD.BaseRegisterGroups);
}

INSTANTIATE_TEST_SUITE_P(BurstGramData,
                         BurstGramDataBurstNonConvertibleModesInit,
                         ::testing::Values(BurstMode::Basic,
                                           BurstMode::CustomBurst));

struct BurstGramDataBurstAssertOnNonEmptyGroupingsModesInit
    : public ::testing::TestWithParam<BurstMode> {};

TEST_P(BurstGramDataBurstAssertOnNonEmptyGroupingsModesInit,
       AssertOnEmptyGroupings) {

  BurstGramData BGD = testing::getBurstGramData(GetParam());

#ifndef NDEBUG
  ASSERT_DEBUG_DEATH(
      BGD.convertToCustomMode(testing::getDummyHistogram(),
                              testing::getDummyState().getInstrInfo()),
      R"(Groupings are specified but burst mode is not \\"custom\\")");
#endif
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataBurstAssertOnNonEmptyGroupingsModesInit,
    ::testing::Values(BurstMode::StoreBurst, BurstMode::LoadBurst,
                      BurstMode::MixedBurst, BurstMode::LoadStoreBurst));

struct BurstGramDataConvertingToCustomModeInitData {
  // not a LLVMState, because ::testing::Values requires copy-ctor
  std::string Triple;
  std::string Features;

  OpcodeHistogram Hist;

  BurstMode Mode;

  BurstGramData::UniqueOpcodesTy ExpectedBurstGroup;
};

struct BurstGramDataConvertingToCustomMode
    : public ::testing::TestWithParam<
          BurstGramDataConvertingToCustomModeInitData> {
  BurstGramData getBurstGramDataAfterConversionToCustomMode() {
    BurstGramData BGD =
        testing::getBurstGramData(GetParam().Mode, std::nullopt);
    // Create LLVMState locally since it's not copyable, but
    // ::testing::Values requires it
    LLVMState State =
        testing::getLLVMState(GetParam().Triple, "", "", GetParam().Features);
    BGD.convertToCustomMode(GetParam().Hist, State.getInstrInfo());
    return BGD;
  }
  const BurstGramData::UniqueOpcodesTy &getExpectedBurstGroup() {
    return GetParam().ExpectedBurstGroup;
  }
};

TEST_P(BurstGramDataConvertingToCustomMode, ConvertingToCustomMode) {
  BurstGramData BGD = getBurstGramDataAfterConversionToCustomMode();

  EXPECT_EQ(BGD.Mode, BurstMode::CustomBurst);
  EXPECT_TRUE(BGD.Groupings.has_value());
  EXPECT_EQ(BGD.Groupings->size(), 1u);
  EXPECT_EQ(BGD.Groupings->front(), getExpectedBurstGroup());
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataConvertingToCustomMode,
    ::testing::Values(
        // LoadBurst: only loads, no stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LD, 8.0}, {ADD, 10.0}, {LB, 2.0}, {SW, 6.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{LD, LB}},

        // LoadBurst: no loads at all (empty)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {SUB, 5.0}, {MUL, 4.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{}},

        // LoadBurst: only loads
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LW, 5.0}, {LH, 4.0}, {LB, 2.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{LW, LH, LB}},

        // StoreBurst: only stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {SW, 6.0}, {ADD, 10.0}, {SB, 3.0}, {LD, 8.0}, {SH, 3.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{SW, SB, SH}},

        // StoreBurst: no stores at all (empty)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {SUB, 5.0}, {MUL, 4.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{}},

        // StoreBurst: only stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{SW, 6.0}, {SD, 4.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{SW, SD}},

        // MixedBurst: loads and stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{LD, 8.0},
                                                     {SW, 6.0},
                                                     {ADD, 10.0},
                                                     {LB, 2.0},
                                                     {SD, 4.0},
                                                     {MUL, 4.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{LD, SW, LB, SD}},

        // MixedBurst: only loads
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LD, 8.0}, {LB, 2.0}, {ADD, 10.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{LD, LB}},

        // MixedBurst: only stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {SW, 6.0}, {SD, 4.0}, {ADD, 10.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{SW, SD}},

        // MixedBurst: nothing memory-related
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {SUB, 5.0}, {MUL, 4.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{}},

        // Atomic loads and stores (LR/SC and AMO)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LR_D, 1.0}, {SC_D, 2.0}, {AMOADD_W, 3.0}, {ADD, 10.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{LR_D, SC_D, AMOADD_W}},

        // Histogram with only one instruction
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/SmallVector<std::pair<unsigned, double>>{{LD, 1.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{LD}},

        // Histogram with only one store
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/SmallVector<std::pair<unsigned, double>>{{SW, 1.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{SW}},

        // Histogram with only one non-memory instruction (all modes empty)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/SmallVector<std::pair<unsigned, double>>{{ADD, 1.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{}},

        // riscv32 with loads/stores (32-bit only)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv32",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LW, 5.0}, {SW, 6.0}, {ADD, 10.0}, {LB, 2.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{LW, LB}},

        // riscv32 mixed burst
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv32",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LW, 5.0}, {SW, 6.0}, {ADD, 10.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{LW, SW}},

        // riscv32 store burst (explicitly testing empty loads)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv32",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{SW, 6.0}, {ADD, 10.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{SW}},

        // Float loads and stores (LoadBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FLW, 5.0}, {FLD, 8.0}, {FSD, 4.0}, {FADD_S, 3.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{FLD, FLW}},

        // Float loads and stores (StoreBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FLW, 5.0}, {FSW, 6.0}, {FSD, 4.0}, {FADD_S, 3.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{FSW, FSD}},

        // Float loads and stores (MixedBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FLW, 5.0}, {FSD, 4.0}, {FSW, 6.0}, {FLD, 8.0}, {FADD_S, 3.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{FLD, FSW, FLW, FSD}},

        // Vector loads and stores (LoadBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLE32_V, 4.0}, {VSE64_V, 3.0}, {VADD_VV, 5.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{VLE32_V}},

        // Vector loads and stores (StoreBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLE32_V, 4.0}, {VSE64_V, 3.0}, {VADD_VV, 5.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{VSE64_V}},

        // Vector loads and stores (MixedBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLE32_V, 4.0}, {VSE64_V, 3.0}, {VADD_VV, 5.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/{VLE32_V, VSE64_V}},

        // Segment loads (LoadBurst)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLSEG2E32_V, 4.0}, {VLSEG3E8_V, 3.0}, {VLE32_V, 5.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{VLSEG2E32_V, VLSEG3E8_V, VLE32_V}},

        // Atomic loads (LoadBurst): LR_D is load, SC_D is store (not loaded)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LR_D, 1.0}, {SC_D, 2.0}, {AMOADD_W, 3.0}},
            /*Mode=*/BurstMode::LoadBurst,
            /*ExpectedBurstGroup=*/{LR_D}},

        // Atomic stores (StoreBurst): SC_D and AMOADD_W are stores
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LR_D, 1.0}, {SC_D, 2.0}, {AMOADD_W, 3.0}},
            /*Mode=*/BurstMode::StoreBurst,
            /*ExpectedBurstGroup=*/{SC_D, AMOADD_W}},

        // Mixed extensions (F + A + V)
        BurstGramDataConvertingToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+a,+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{FLD, 8.0},
                                                     {AMOADD_W, 3.0},
                                                     {VLE32_V, 4.0},
                                                     {FSD, 4.0},
                                                     {SC_D, 2.0},
                                                     {VSE64_V, 3.0}},
            /*Mode=*/BurstMode::MixedBurst,
            /*ExpectedBurstGroup=*/
            {FLD, AMOADD_W, VLE32_V, FSD, SC_D, VSE64_V}}));

struct BurstGramDataConvertingLoadStoreBurstToCustomModeInitData {
  // not a LLVMState, because ::testing::Values requires copy-ctor
  std::string Triple;
  std::string Features;

  OpcodeHistogram Hist;

  BurstGramData::UniqueOpcodesTy ExpectedStoreBurstGroup;
  BurstGramData::UniqueOpcodesTy ExpectedLoadBurstGroup;
};

struct BurstGramDataConvertingLoadStoreBurstToCustomMode
    : public ::testing::TestWithParam<
          BurstGramDataConvertingLoadStoreBurstToCustomModeInitData> {
  BurstGramData getBurstGramDataAfterConversionToCustomMode() {
    BurstGramData BGD =
        testing::getBurstGramData(BurstMode::LoadStoreBurst, std::nullopt);
    // Create LLVMState locally since it's not copyable, but
    // ::testing::Values requires it
    LLVMState State =
        testing::getLLVMState(GetParam().Triple, "", "", GetParam().Features);
    BGD.convertToCustomMode(GetParam().Hist, State.getInstrInfo());
    return BGD;
  }
  const BurstGramData::UniqueOpcodesTy &getExpectedLoadBurstGroup() {
    return GetParam().ExpectedLoadBurstGroup;
  }
  const BurstGramData::UniqueOpcodesTy &getExpectedStoreBurstGroup() {
    return GetParam().ExpectedStoreBurstGroup;
  }
};

TEST_P(BurstGramDataConvertingLoadStoreBurstToCustomMode,
       ConvertingToCustomMode) {
  BurstGramData BGD = getBurstGramDataAfterConversionToCustomMode();
  EXPECT_EQ(BGD.Mode, BurstMode::CustomBurst);
  EXPECT_TRUE(BGD.Groupings.has_value());
  EXPECT_EQ(BGD.Groupings->size(), 2u);
  EXPECT_EQ(BGD.Groupings.value()[0], getExpectedStoreBurstGroup());
  EXPECT_EQ(BGD.Groupings.value()[1], getExpectedLoadBurstGroup());
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataConvertingLoadStoreBurstToCustomMode,
    ::testing::Values(
        // Only loads and stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LD, 8.0}, {ADD, 10.0}, {LB, 2.0}, {SW, 6.0}},
            /*ExpectedStoreBurstGroup=*/{SW},
            /*ExpectedLoadBurstGroup=*/{LD, LB}},

        // Only stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {SW, 6.0}, {SD, 4.0}},
            /*ExpectedStoreBurstGroup=*/{SW, SD},
            /*ExpectedLoadBurstGroup=*/{}},

        // Only loads
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {LW, 5.0}, {LH, 4.0}},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{LW, LH}},

        // Neither loads nor stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {ADD, 10.0}, {SUB, 5.0}, {MUL, 4.0}},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{}},

        // Loads and stores mixed randomly with arithmetic
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{ADD, 10.0},
                                                     {LD, 8.0},
                                                     {SUB, 5.0},
                                                     {SW, 6.0},
                                                     {LB, 2.0},
                                                     {SD, 4.0}},
            /*ExpectedStoreBurstGroup=*/{SW, SD},
            /*ExpectedLoadBurstGroup=*/{LD, LB}},

        // Atomic: AMOADD_W is both load and store, so it goes to stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LR_D, 1.0}, {SC_D, 2.0}, {AMOADD_W, 3.0}, {ADD, 10.0}},
            /*ExpectedStoreBurstGroup=*/{SC_D, AMOADD_W},
            /*ExpectedLoadBurstGroup=*/{LR_D}},

        // Floating-point loads and stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FLD, 8.0}, {FSW, 6.0}, {FLW, 5.0}, {FSD, 4.0}, {FADD_S, 3.0}},
            /*ExpectedStoreBurstGroup=*/{FSW, FSD},
            /*ExpectedLoadBurstGroup=*/{FLD, FLW}},

        // Vector loads and stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLE32_V, 4.0}, {VSE64_V, 3.0}, {VADD_VV, 5.0}},
            /*ExpectedStoreBurstGroup=*/{VSE64_V},
            /*ExpectedLoadBurstGroup=*/{VLE32_V}},

        // Single load
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{LD, 1.0}},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{LD}},

        // Single store
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{SW, 1.0}},
            /*ExpectedStoreBurstGroup=*/{SW},
            /*ExpectedLoadBurstGroup=*/{}},

        // Empty histogram
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/SmallVector<std::pair<unsigned, double>>{},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{}},

        // riscv32 loads and stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv32",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LW, 5.0}, {SW, 6.0}, {LB, 2.0}, {ADD, 10.0}},
            /*ExpectedStoreBurstGroup=*/{SW},
            /*ExpectedLoadBurstGroup=*/{LW, LB}},

        // Store and load of different widths
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{LB, 2.0},
                                                     {LH, 3.0},
                                                     {LW, 4.0},
                                                     {LD, 5.0},
                                                     {SB, 2.0},
                                                     {SH, 3.0},
                                                     {SW, 4.0},
                                                     {SD, 5.0}},
            /*ExpectedStoreBurstGroup=*/{SB, SH, SW, SD},
            /*ExpectedLoadBurstGroup=*/{LB, LH, LW, LD}},

        // Only AMO instructions (which are both load and store)
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{AMOADD_W, 2.0},
                                                     {AMOSWAP_D, 3.0}},
            /*ExpectedStoreBurstGroup=*/{AMOADD_W, AMOSWAP_D},
            /*ExpectedLoadBurstGroup=*/{}},

        // Vector + Atomic mixed
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v,+a",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {VLE32_V, 4.0}, {AMOADD_W, 3.0}, {VSE64_V, 2.0}, {LR_D, 1.0}},
            /*ExpectedStoreBurstGroup=*/{AMOADD_W, VSE64_V},
            /*ExpectedLoadBurstGroup=*/{VLE32_V, LR_D}},

        // Only float loads
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FLW, 5.0}, {FLD, 8.0}, {FADD_S, 3.0}},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{FLD, FLW}},

        // Only float stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+f,+d",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {FSW, 6.0}, {FSD, 4.0}, {FADD_S, 3.0}},
            /*ExpectedStoreBurstGroup=*/{FSW, FSD},
            /*ExpectedLoadBurstGroup=*/{}},

        // Only vector loads
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{VLE32_V, 4.0},
                                                     {VADD_VV, 5.0}},
            /*ExpectedStoreBurstGroup=*/{},
            /*ExpectedLoadBurstGroup=*/{VLE32_V}},

        // Only vector stores
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{VSE64_V, 3.0},
                                                     {VADD_VV, 5.0}},
            /*ExpectedStoreBurstGroup=*/{VSE64_V},
            /*ExpectedLoadBurstGroup=*/{}},

        // riscv32 with C extension (compressed) - C_SW is store, C_LW is load
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv32",
            /*Features=*/"+c",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{{C_LW, 5.0}, {C_SW, 6.0}},
            /*ExpectedStoreBurstGroup=*/{C_SW},
            /*ExpectedLoadBurstGroup=*/{C_LW}},

        // Max loads, 1 store
        BurstGramDataConvertingLoadStoreBurstToCustomModeInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*Hist=*/
            SmallVector<std::pair<unsigned, double>>{
                {LB, 2.0}, {LH, 3.0}, {LW, 4.0}, {LD, 5.0}, {SW, 1.0}},
            /*ExpectedStoreBurstGroup=*/{SW},
            /*ExpectedLoadBurstGroup=*/{LB, LH, LW, LD}}));

struct BurstGramDataMappingOpcodesToNumBurstGroupsInitData {
  BurstGramData::OpcodeGroupsTy OpcodeGroups;
  BurstGramData::OpcodeToNumGroupsTy ExpectedOpcodeToNumGroups;
};

struct MappingOpcodesToNumBurstGroups
    : public ::testing::TestWithParam<
          BurstGramDataMappingOpcodesToNumBurstGroupsInitData> {
  BurstGramData::OpcodeToNumGroupsTy getBurstGramDataMapping() {
    return testing::getBurstGramData(BurstMode::CustomBurst,
                                     GetParam().OpcodeGroups)
        .getOpcodeToNumBurstGroups();
  }
  const BurstGramData::OpcodeToNumGroupsTy &getExpectedMapping() {
    return GetParam().ExpectedOpcodeToNumGroups;
  }
};

TEST_P(MappingOpcodesToNumBurstGroups, BurstGramData) {
  BurstGramData::OpcodeToNumGroupsTy BGDMapping = getBurstGramDataMapping();
  const BurstGramData::OpcodeToNumGroupsTy &ExpectedMapping =
      getExpectedMapping();
  EXPECT_EQ(BGDMapping, ExpectedMapping);
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, MappingOpcodesToNumBurstGroups,
    ::testing::Values(
        // Simple disjoint groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD}, {SUB, ADDI}},
            /*OpcodeToNumGroups=*/{{ADD, 1u}, {SUB, 1u}, {ADDI, 1u}}},

        // Partial overlap (ADD appears in two groups)
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD}, {SUB, ADD}},
            /*OpcodeToNumGroups=*/{{ADD, 2u}, {SUB, 1u}}},

        // Identical groups (all instructions appear twice)
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {ADD, SUB}},
            /*OpcodeToNumGroups=*/{{ADD, 2u}, {SUB, 2u}}},

        // Repeated single-element groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADDI}, {ADDI}, {ADDI}},
            /*OpcodeToNumGroups=*/{{ADDI, 3u}}},

        // Complex overlap across three groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{MUL, ADD}, {DIV, MUL}, {ADD, MUL, DIV}},
            /*OpcodeToNumGroups=*/{{ADD, 2u}, {MUL, 3u}, {DIV, 2u}}},

        // One large group and one small group fully contained in it
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL}, {SUB}},
            /*OpcodeToNumGroups=*/{{ADD, 1u}, {SUB, 2u}, {MUL, 1u}}},

        // One empty group and one regular group
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{}, {ADD, SUB}},
            /*OpcodeToNumGroups=*/{{ADD, 1u}, {SUB, 1u}}},

        // Both groups are empty
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{}, {}},
            /*OpcodeToNumGroups=*/{}},

        // Group fully covers all previous groups and adds new
        // instructions
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {MUL, DIV}, {ADD, SUB, MUL, DIV}},
            /*OpcodeToNumGroups=*/{{ADD, 2u}, {SUB, 2u}, {MUL, 2u}, {DIV, 2u}}},

        // Alternating overlapping groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {SUB, MUL}, {MUL, DIV}},
            /*OpcodeToNumGroups=*/{{ADD, 1u}, {SUB, 2u}, {MUL, 2u}, {DIV, 1u}}},

        // Large group combined with several singletons
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL, DIV}, {ADD}, {SUB}, {DIV}},
            /*OpcodeToNumGroups=*/{{ADD, 2u}, {SUB, 2u}, {MUL, 1u}, {DIV, 2u}}},

        // Multiple identical groups with more than two elements
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{
                {ADD, SUB, MUL}, {ADD, SUB, MUL}, {ADD, SUB, MUL}},
            /*OpcodeToNumGroups=*/{{ADD, 3u}, {SUB, 3u}, {MUL, 3u}}},

        // Empty group interspersed between non-empty groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD}, {}, {SUB}, {}},
            /*OpcodeToNumGroups=*/{{ADD, 1u}, {SUB, 1u}}},

        // Intersection of two large mostly disjoint groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL, DIV, REM},
                              {AND, OR, XOR, ADD, SUB}},
            /*OpcodeToNumGroups=*/{{ADD, 2u},
                                   {SUB, 2u},
                                   {MUL, 1u},
                                   {DIV, 1u},
                                   {REM, 1u},
                                   {AND, 1u},
                                   {OR, 1u},
                                   {XOR, 1u}}},

        // Chain of overlapping groups where each group shares one
        // element with the previous one
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {SUB, MUL}, {MUL, DIV}, {DIV, REM}},
            /*OpcodeToNumGroups=*/{{ADD, 1u},
                                   {SUB, 2u},
                                   {MUL, 2u},
                                   {DIV, 2u},
                                   {REM, 1u}}},

        // Single huge group containing many instructions
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL, DIV, REM, AND, OR, XOR}},
            /*OpcodeToNumGroups=*/{{ADD, 1u},
                                   {SUB, 1u},
                                   {MUL, 1u},
                                   {DIV, 1u},
                                   {REM, 1u},
                                   {AND, 1u},
                                   {OR, 1u},
                                   {XOR, 1u}}},

        // Groups with single common instruction repeated many times
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {ADD, MUL}, {ADD, DIV}, {ADD, REM}},
            /*OpcodeToNumGroups=*/{{ADD, 4u},
                                   {SUB, 1u},
                                   {MUL, 1u},
                                   {DIV, 1u},
                                   {REM, 1u}}},

        // Nested groups (each subsequent is a subset of the
        // previous)
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL}, {ADD, SUB}, {ADD}},
            /*OpcodeToNumGroups=*/{{ADD, 3u}, {SUB, 2u}, {MUL, 1u}}},

        // Completely disjoint multi-element groups
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {MUL, DIV}, {AND, OR}},
            /*OpcodeToNumGroups=*/{{ADD, 1u},
                                   {SUB, 1u},
                                   {MUL, 1u},
                                   {DIV, 1u},
                                   {AND, 1u},
                                   {OR, 1u}}},

        // Every instruction appears in every group
        BurstGramDataMappingOpcodesToNumBurstGroupsInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL},
                              {ADD, SUB, MUL},
                              {ADD, SUB, MUL},
                              {ADD, SUB, MUL}},
            /*OpcodeToNumGroups=*/{{ADD, 4u}, {SUB, 4u}, {MUL, 4u}}}));

TEST(BurstGramDataAllBurstOpcodes, NulloptGroupings) {
  BurstGramData BGD =
      testing::getBurstGramData(BurstMode::CustomBurst, std::nullopt);
  BurstGramData::UniqueOpcodesTy UniqueOpcodes = BGD.getAllBurstOpcodes();
  EXPECT_TRUE(UniqueOpcodes.empty());
}

TEST(BurstGramDataOpcodeToNumBurstGroups, NulloptGroupings) {
  BurstGramData BGD =
      testing::getBurstGramData(BurstMode::CustomBurst, std::nullopt);
#ifndef NDEBUG
  ASSERT_DEBUG_DEATH(BGD.getOpcodeToNumBurstGroups(), "Groupings");
#endif
}

struct BurstGramDataAllGroupsOpcodesInitData {
  BurstGramData::OpcodeGroupsTy OpcodeGroups;
  BurstGramData::UniqueOpcodesTy ExpectedAllBurstOpcodes;
};

struct BurstGramDataAllBurstOpcodes
    : public ::testing::TestWithParam<BurstGramDataAllGroupsOpcodesInitData> {
  BurstGramData::UniqueOpcodesTy getBurstGramDataAllBurstOpcodes() {
    return testing::getBurstGramData(BurstMode::CustomBurst,
                                     GetParam().OpcodeGroups)
        .getAllBurstOpcodes();
  }

  const BurstGramData::UniqueOpcodesTy &getExpectedAllBurstOpcodes() {
    return GetParam().ExpectedAllBurstOpcodes;
  }
};

TEST_P(BurstGramDataAllBurstOpcodes, NonNulloptGroupings) {
  BurstGramData::UniqueOpcodesTy BGDAllBurstOpcodes =
      getBurstGramDataAllBurstOpcodes();
  const BurstGramData::UniqueOpcodesTy &ExpectedAllBurstOpcodes =
      getExpectedAllBurstOpcodes();

  EXPECT_EQ(BGDAllBurstOpcodes, ExpectedAllBurstOpcodes);
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataAllBurstOpcodes,
    ::testing::Values(
        // All groups identical (single unique set)
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL},
                              {ADD, SUB, MUL},
                              {ADD, SUB, MUL},
                              {ADD, SUB, MUL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}},

        // Single group with multiple opcodes
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, ADDI}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, ADDI}},

        // Single group with a single opcode
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}},
            /*ExpectedAllBurstOpcodes=*/{ADD}},

        // Disjoint groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}, {SUB}, {MUL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}},

        // Partial overlap (ADD repeats, rest unique)
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {ADD, MUL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}},

        // Fully overlapping groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {ADD, SUB}, {ADD, SUB}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB}},

        // Groups with empty sets interspersed
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}, {}, {SUB}, {}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB}},

        // Empty input groups list
        BurstGramDataAllGroupsOpcodesInitData{/*OpcodeGroups=*/{},
                                              /*ExpectedAllBurstOpcodes=*/{}},

        // Only one empty group
        BurstGramDataAllGroupsOpcodesInitData{/*OpcodeGroups=*/{{}},
                                              /*ExpectedAllBurstOpcodes=*/{}},

        // Multiple empty groups
        BurstGramDataAllGroupsOpcodesInitData{/*OpcodeGroups=*/{{}, {}, {}},
                                              /*ExpectedAllBurstOpcodes=*/{}},

        // Chain of overlapping groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {SUB, MUL}, {MUL, DIV}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV}},

        // Large groups with a small intersection
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL, DIV}, {MUL, DIV, REM, AND}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM, AND}},

        // Groups of varying sizes
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}, {SUB, MUL}, {DIV, REM, AND}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM, AND}},

        // Nested groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL}, {ADD, SUB}, {ADD}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}},

        // Completely disjoint groups of different sizes
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}, {SUB, MUL}, {DIV, REM, AND}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM, AND}},

        // Alternating disjoint and empty groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD}, {}, {SUB}, {}, {MUL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}},

        // Repeated single-element groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADDI}, {ADDI}, {ADDI}},
            /*ExpectedAllBurstOpcodes=*/{ADDI}},

        // Many groups sharing exactly one instruction
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {ADD, MUL}, {ADD, DIV}, {ADD, REM}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM}},

        // Empty group first, then non-empty
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{}, {ADD, SUB}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB}},

        // Empty group last, after non-empty
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB}, {}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB}},

        // Complex overlap with multiple duplicate instructions
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{
                {ADD, SUB, MUL}, {SUB, MUL, DIV}, {MUL, DIV, REM}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM}},

        // Two completely disjoint large groups
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL, DIV, REM},
                              {AND, OR, XOR, SLL, SRL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL, DIV, REM, AND, OR, XOR,
                                         SLL, SRL}},

        // One large group and several singletons
        BurstGramDataAllGroupsOpcodesInitData{
            /*OpcodeGroups=*/{{ADD, SUB, MUL}, {ADD}, {SUB}, {MUL}},
            /*ExpectedAllBurstOpcodes=*/{ADD, SUB, MUL}}));

struct BurstGramDataRemoveUnsupportedOpcodesInitData {
  // not a LLVMState, because ::testing::Values requires copy-ctor
  std::string Triple;
  std::string Features;

  BurstGramData::OpcodeGroupsTy PrimaryOpcodeGroups;
  BurstGramData::OpcodeGroupsTy ExpectedGroupingsAfterRemovingUnsupported;

  std::vector<std::string> ExpectedWarnings;
};

struct BurstGramDataRemoveUnsupportedOpcodes
    : public ::testing::TestWithParam<
          BurstGramDataRemoveUnsupportedOpcodesInitData> {
  BurstGramData::OpcodeGroupsTy
  getBurstGramDataGroupingsWithRemovedUnusedOpcodes() {
    // Create LLVMState locally since it's not copyable, but
    // ::testing::Values requires it
    LLVMState State =
        testing::getLLVMState(GetParam().Triple, "", "", GetParam().Features);

    BurstGramData BGD = testing::getBurstGramData(
        BurstMode::CustomBurst, GetParam().PrimaryOpcodeGroups);
    OpcodeCache OpCC = testing::getOpcodeCache(State);

    BGD.removeUnsupportedOpcodes(State, OpCC);
    return *BGD.Groupings;
  }

  const BurstGramData::OpcodeGroupsTy &
  getExpectedGroupingAfterRemovingUnsupported() {
    return GetParam().ExpectedGroupingsAfterRemovingUnsupported;
  }
};

MATCHER_P(HasAllSubstrings, Substrings, "") {
  std::string Str = arg;
  for (const auto &Sub : Substrings) {
    if (Str.find(Sub) == std::string::npos) {
      *result_listener << "missing substring: \"" << Sub << "\"";
      return false;
    }
  }
  *result_listener << "all substrings found";
  return true;
}

// FIXME: restore instruction names in expected warning messages once the bug
// causing warning message to return an empty instruction name

TEST_P(BurstGramDataRemoveUnsupportedOpcodes, BurstGramData) {
  ::testing::internal::CaptureStderr();

  BurstGramData::OpcodeGroupsTy BGDGroupings =
      getBurstGramDataGroupingsWithRemovedUnusedOpcodes();

  EXPECT_THAT(::testing::internal::GetCapturedStderr(),
              HasAllSubstrings(GetParam().ExpectedWarnings));

  const BurstGramData::OpcodeGroupsTy &ExpectedGroupings =
      getExpectedGroupingAfterRemovingUnsupported();

  EXPECT_EQ(BGDGroupings, ExpectedGroupings);
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataRemoveUnsupportedOpcodes,
    ::testing::Values(
        // Arithmetic survives, VSETIVLI is removed (name not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD, SUB, VSETIVLI, MUL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB, MUL}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // All mode switch instructions are removed (names not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{VSETVL, VSETVLI, VSETIVLI}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"is not supported", "Empty burst group"}},

        // Jumps and calls (JAL, JALR printed; C_JAL, C_JALR not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, JAL, SUB}, {JALR, C_JAL, C_JALR}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}, {}},
            /*ExpectedWarnings=*/
            {"JAL", "JALR", "is not supported", "Empty burst group"}},

        // Conditional branches (BEQ, BNE printed; C_BEQZ, C_BNEZ
        // not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, BEQ, BNE, SUB}, {C_BEQZ, C_BNEZ}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}, {}},
            /*ExpectedWarnings=*/
            {"BEQ", "BNE", "is not supported", "Empty burst group"}},

        // Return instructions (MRET, SRET, DRET printed; PseudoRET
        // not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, MRET, SUB}, {SRET, PseudoRET}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}, {}},
            /*ExpectedWarnings=*/
            {"MRET", "SRET", "is not supported", "Empty burst group"}},

        // Zcmp SP-relative (names not printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, CM_PUSH, CM_POP}, {CM_POPRET, CM_POPRETZ}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD}, {}},
            /*ExpectedWarnings=*/
            {"is not supported", "Empty burst group"}},

        // Mixed group (JAL, BEQ printed; VSETIVLI, CM_PUSH not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, SUB, MUL}, {VSETIVLI, JAL, BEQ, CM_PUSH}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB, MUL}, {}},
            /*ExpectedWarnings=*/
            {"JAL", "BEQ", "is not supported", "Empty burst group"}},

        // Empty groups are kept as-is (no warnings)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{}, {}, {}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{}, {}, {}},
            /*ExpectedWarnings=*/{}},

        // Empty input list (no warnings)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{},
            /*ExpectedWarnings=*/{}},

        // Group consisting entirely of unsupported instructions
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{BEQ, BNE, JAL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"BEQ", "BNE", "JAL", "Empty burst group"}},

        // Single empty group input (no warnings)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/{}},

        // Large mixed group (BEQ, JAL printed; VSETVL, CM_PUSH not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, SUB, MUL, DIV, AND, OR, XOR, VSETVL, BEQ, JAL, CM_PUSH}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB, MUL, DIV, AND, OR, XOR}},
            /*ExpectedWarnings=*/
            {"BEQ", "JAL", "is not supported"}},

        // Only jumps and calls (JAL, JALR printed; C_JAL, C_JALR,
        // CM_JALT not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{JAL, JALR, C_JAL, C_JALR, CM_JALT}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"JAL", "JALR", "is not supported", "Empty burst group"}},

        // Only branches (all printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{BEQ, BNE, BLT, BGE, BLTU, BGEU}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"BEQ", "BNE", "BLT", "BGE", "BLTU", "BGEU", "Empty burst group"}},

        // Mix of all control flow (JAL, BEQ, MRET printed; CM_PUSH,
        // VSETVL not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{JAL, BEQ, MRET, CM_PUSH, VSETVL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"JAL", "BEQ", "MRET", "is not supported", "Empty burst group"}},

        // Multiple groups, each with a single surviving instruction
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, JAL}, {SUB, BEQ}, {MUL, MRET}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD}, {SUB}, {MUL}},
            /*ExpectedWarnings=*/{"JAL", "BEQ", "MRET"}},

        // Multiple groups, each fully removed (CM_PUSH not printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{JAL, JALR}, {BEQ, BNE}, {MRET, CM_PUSH}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{}, {}, {}},
            /*ExpectedWarnings=*/
            {"JAL", "JALR", "BEQ", "BNE", "MRET", "is not supported",
             "Empty burst group"}},

        // Alternating surviving and fully removed (CM_POP not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD}, {JAL, BEQ}, {SUB}, {MRET, CM_POP}, {MUL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD}, {}, {SUB}, {}, {MUL}},
            /*ExpectedWarnings=*/
            {"JAL", "BEQ", "MRET", "is not supported", "Empty burst group"}},

        // Large arithmetic group with a single control flow (name
        // not printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{ADD, SUB, MUL, DIV, REM, AND, OR, XOR, SLL, SRL, SRA, VSETIVLI}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB, MUL, DIV, REM, AND, OR, XOR, SLL, SRL, SRA}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // Surviving instructions before and after a removed one
        // (name not printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD, VSETIVLI, SUB}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // Duplicates of removed instructions
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD, JAL, JAL, JAL, SUB}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}},
            /*ExpectedWarnings=*/{"JAL"}},

        // Only one instruction, and it is removed
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{JAL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/{"JAL", "Empty burst group"}},

        // Only one instruction, and it survives (no warnings)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{ADD}},
            /*ExpectedWarnings=*/{}},

        // Empty group in the middle of a sequence (no warnings)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD, SUB}, {}, {MUL, DIV}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, SUB}, {}, {MUL, DIV}},
            /*ExpectedWarnings=*/{}},

        // Empty group between two fully removed groups (CM_PUSH not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{JAL, BEQ}, {}, {MRET, CM_PUSH}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{}, {}, {}},
            /*ExpectedWarnings=*/
            {"JAL", "BEQ", "MRET", "is not supported", "Empty burst group"}},

        // All possible Zcmp SP-relative instructions (names not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{CM_PUSH, CM_POP, CM_POPRET, CM_POPRETZ, ADD}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // Mix of RVV mode switch with arithmetic (names not
        // printed)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/
            {{VADD_VV, VSETVLI, VMUL_VX, VSETVL}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{VADD_VV, VMUL_VX}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // MRET and SRET are removed (MRET, SRET, DRET printed;
        // PseudoRET not)
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{MRET, SRET, DRET, PseudoRET}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/{{}},
            /*ExpectedWarnings=*/
            {"MRET", "SRET", "DRET", "is not supported", "Empty burst group"}},

        // MNRET is NOT a return instruction (no Return flag), so it
        // survives
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"",
            /*PrimaryOpcodeGroups=*/{{ADD, MNRET, SUB}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, MNRET, SUB}},
            /*ExpectedWarnings=*/{}},

        // Test with +c (compressed): C_ADDI survives, C_JAL is removed
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+c",
            /*PrimaryOpcodeGroups=*/{{ADD, C_ADDI, C_JAL, C_LW}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{ADD, C_ADDI, C_LW}},
            /*ExpectedWarnings=*/{"is not supported"}},

        // Test with +v (vector): VSETIVLI is removed, VADD_VV survives
        BurstGramDataRemoveUnsupportedOpcodesInitData{
            /*Triple=*/"riscv64",
            /*Features=*/"+v",
            /*PrimaryOpcodeGroups=*/{{VADD_VV, VSETIVLI, VMUL_VX}},
            /*ExpectedGroupingsAfterRemovingUnsupported=*/
            {{VADD_VV, VMUL_VX}},
            /*ExpectedWarnings=*/{"is not supported"}}));

// The following test verifies that the BurstGramData methods do not depend
// on the state of BaseRegisterGroups and do not modify it.

struct BurstGramDataIndependsOnBaseRegisterGroupsData {
  std::optional<BurstGramData::OpcodeGroupsTy> FirstBaseRegisterGroups;
  std::optional<BurstGramData::OpcodeGroupsTy> SecondBaseRegisterGroups;
};

struct BurstGramDataIndependsOnBaseRegisterGroups
    : public ::testing::TestWithParam<
          BurstGramDataIndependsOnBaseRegisterGroupsData> {};

TEST_P(BurstGramDataIndependsOnBaseRegisterGroups, BaseRegisterGroups) {
  {
    BurstGramData LHS =
        testing::getBurstGramData(GetParam().FirstBaseRegisterGroups);
    BurstGramData RHS =
        testing::getBurstGramData(GetParam().SecondBaseRegisterGroups);

    auto OldLHSBaseRegisterGroups = LHS.BaseRegisterGroups;
    auto OldRHSBaseRegisterGroups = RHS.BaseRegisterGroups;

    auto LHSMapping = LHS.getOpcodeToNumBurstGroups();
    auto RHSMapping = RHS.getOpcodeToNumBurstGroups();

    EXPECT_EQ(LHSMapping, RHSMapping);

    EXPECT_EQ(LHS.Mode, RHS.Mode);
    EXPECT_EQ(LHS.MinSize, RHS.MinSize);
    EXPECT_EQ(LHS.MaxSize, RHS.MaxSize);
    EXPECT_EQ(LHS.Groupings, RHS.Groupings);

    EXPECT_EQ(LHS.BaseRegisterGroups, OldLHSBaseRegisterGroups);
    EXPECT_EQ(RHS.BaseRegisterGroups, OldRHSBaseRegisterGroups);
  }
  {
    BurstGramData LHS =
        testing::getBurstGramData(GetParam().FirstBaseRegisterGroups);
    BurstGramData RHS =
        testing::getBurstGramData(GetParam().SecondBaseRegisterGroups);

    auto OldLHSBaseRegisterGroups = LHS.BaseRegisterGroups;
    auto OldRHSBaseRegisterGroups = RHS.BaseRegisterGroups;

    auto LHSOpcodes = LHS.getAllBurstOpcodes();
    auto RHSOpcodes = RHS.getAllBurstOpcodes();

    EXPECT_EQ(LHSOpcodes, RHSOpcodes);

    EXPECT_EQ(LHS.Mode, RHS.Mode);
    EXPECT_EQ(LHS.MinSize, RHS.MinSize);
    EXPECT_EQ(LHS.MaxSize, RHS.MaxSize);
    EXPECT_EQ(LHS.Groupings, RHS.Groupings);

    EXPECT_EQ(LHS.BaseRegisterGroups, OldLHSBaseRegisterGroups);
    EXPECT_EQ(RHS.BaseRegisterGroups, OldRHSBaseRegisterGroups);
  }
  {
    BurstGramData LHS =
        testing::getBurstGramData(GetParam().FirstBaseRegisterGroups);
    BurstGramData RHS =
        testing::getBurstGramData(GetParam().SecondBaseRegisterGroups);

    auto OldLHSBaseRegisterGroups = LHS.BaseRegisterGroups;
    auto OldRHSBaseRegisterGroups = RHS.BaseRegisterGroups;

    auto State = testing::getDummyState();
    auto OpCC = testing::getOpcodeCache(State);

    LHS.removeUnsupportedOpcodes(State, OpCC);
    RHS.removeUnsupportedOpcodes(State, OpCC);

    EXPECT_EQ(LHS.Mode, RHS.Mode);
    EXPECT_EQ(LHS.MinSize, RHS.MinSize);
    EXPECT_EQ(LHS.MaxSize, RHS.MaxSize);
    EXPECT_EQ(LHS.Groupings, RHS.Groupings);

    EXPECT_EQ(LHS.BaseRegisterGroups, OldLHSBaseRegisterGroups);
    EXPECT_EQ(RHS.BaseRegisterGroups, OldRHSBaseRegisterGroups);
  }
  {
    BurstGramData LHS =
        testing::getBurstGramData(GetParam().FirstBaseRegisterGroups);
    BurstGramData RHS =
        testing::getBurstGramData(GetParam().SecondBaseRegisterGroups);

    auto OldLHSBaseRegisterGroups = LHS.BaseRegisterGroups;
    auto OldRHSBaseRegisterGroups = RHS.BaseRegisterGroups;

    auto State = testing::getDummyState();

    auto Histogram = testing::getDummyHistogram();
    const auto &InstrInfo = State.getInstrInfo();

    LHS.convertToCustomMode(Histogram, InstrInfo);
    RHS.convertToCustomMode(Histogram, InstrInfo);

    EXPECT_EQ(LHS.Mode, RHS.Mode);
    EXPECT_EQ(LHS.MinSize, RHS.MinSize);
    EXPECT_EQ(LHS.MaxSize, RHS.MaxSize);
    EXPECT_EQ(LHS.Groupings, RHS.Groupings);

    EXPECT_EQ(LHS.BaseRegisterGroups, OldLHSBaseRegisterGroups);
    EXPECT_EQ(RHS.BaseRegisterGroups, OldRHSBaseRegisterGroups);
  }
}

INSTANTIATE_TEST_SUITE_P(
    BurstGramData, BurstGramDataIndependsOnBaseRegisterGroups,
    ::testing::Values(
        // Both base register groups are absent
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/std::nullopt,
            /*SecondBaseRegisterGroups=*/std::nullopt},

        // First is absent, second is present
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/std::nullopt,
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, SUB, AND},
                                          {MUL, DIV, REM},
                                          {FADD_D, FMUL_S, FSD},
                                          {VADD_VV, VMUL_VX, VLE32_V},
                                          {LD, SW, AMOADD_W}}},

        // First is present, second is absent
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, SUB, AND},
                                          {MUL, DIV, REM},
                                          {FADD_D, FMUL_S, FSD},
                                          {VADD_VV, VMUL_VX, VLE32_V},
                                          {LD, SW, AMOADD_W}},
            /*SecondBaseRegisterGroups=*/std::nullopt},

        // Both are present but different
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, SUB}, {MUL}},
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{LD, SW}, {VADD_VV}}},

        // Both are present and identical
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, SUB}, {MUL}},
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, SUB}, {MUL}}},

        // Both are empty containers (not nullopt, but empty)
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/BurstGramData::OpcodeGroupsTy{},
            /*SecondBaseRegisterGroups=*/BurstGramData::OpcodeGroupsTy{}},

        // First is empty container, second is absent
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/BurstGramData::OpcodeGroupsTy{},
            /*SecondBaseRegisterGroups=*/std::nullopt},

        // First is absent, second is empty container
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/std::nullopt,
            /*SecondBaseRegisterGroups=*/BurstGramData::OpcodeGroupsTy{}},

        // First has groups with one opcode, second has groups with many
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD}, {SUB}},
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{AND, OR, XOR, SLL, SRL},
                                          {LD, LB, LW, LH}}},

        // Both contain groups that are entirely unsupported
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{VSETIVLI, JAL}},
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{BEQ, CM_PUSH}}},

        // Both contain groups with a mix of supported and unsupported
        BurstGramDataIndependsOnBaseRegisterGroupsData{
            /*FirstBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{ADD, VSETIVLI}, {JAL, MUL}},
            /*SecondBaseRegisterGroups=*/
            BurstGramData::OpcodeGroupsTy{{BEQ, SUB}, {CM_PUSH, DIV}}}));

} // namespace llvm::snippy
