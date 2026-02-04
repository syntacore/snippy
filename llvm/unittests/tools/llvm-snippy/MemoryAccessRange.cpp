#include "snippy/Config/MemoryScheme.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;

struct MemoryRangeTestCase {
  MemoryAccessRange MR;

  template <typename... MRArgs>
  MemoryRangeTestCase(MRArgs... Args) : MR(Args...){};
};

class MemoryRangesTest : public ::testing::TestWithParam<MemoryRangeTestCase> {
protected:
  void assertValidAddress(AddressInfo Res, const MemoryAccessRange &MR,
                          const AddressGenInfo &GenInfo) const {
    auto Start = MR.Start;
    auto Size = MR.Size;
    auto Stride = MR.Stride;
    ASSERT_EQ(Res.MinStride % Stride, 0u);
    if (GenInfo.MinStride)
      ASSERT_EQ(Res.MinStride % GenInfo.MinStride, 0u);
    auto FirstOff = MR.FirstOffset;
    auto LastOff = MR.LastOffset;
    auto MaxAccSize = MR.AccessSize;
    auto MaxOffset = MR.MaxPastLastOffset;

    auto Addr = Res.Address;
    auto Offset = (Addr - Start) % Stride;
    auto AISize = Res.AccessSize;

    ASSERT_GE(Offset, FirstOff);
    ASSERT_LE(Offset, LastOff);
    ASSERT_GE(Addr, Start);
    ASSERT_LE(Addr + AISize, Start + Size);

    if (MaxAccSize)
      ASSERT_LE(AISize, *MaxAccSize);
    if (MaxOffset)
      ASSERT_LE(Offset + AISize, LastOff + 1 + *MaxOffset);
  }
};

TEST_P(MemoryRangesTest, GetRandomAddressTest) {
  static std::vector<AddressGenInfo> InstrAccesses = {
      {/* AccessSize */ 1u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 1u, /* Alignment */ 2u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 1u, /* Alignment */ 4u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 2u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 2u, /* Alignment */ 2u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 2u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 4u, /* AllowMisalign */ true,
       /* BurstMode */ false},
      {/* AccessSize */ 1u, /* Alignment */ 1u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 1u, /* Alignment */ 2u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 1u, /* Alignment */ 4u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 2u, /* Alignment */ 1u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 2u, /* Alignment */ 2u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 1u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 2u, /* AllowMisalign */ false,
       /* BurstMode */ false},
      {/* AccessSize */ 4u, /* Alignment */ 4u, /* AllowMisalign */ false,
       /* BurstMode */ false},
  };
  auto TestCase = GetParam();
  auto &MR = TestCase.MR;

  RandEngine::init(1);
  llvm::for_each(InstrAccesses, [&](const auto &GenInfo) {
    const auto &Res = MR.randomAddress(GenInfo);
    assertValidAddress(Res, MR, GenInfo);
  });
}

TEST_P(MemoryRangesTest, MultipleDoesntExceedSize) {
  static const std::vector<AddressGenInfo> InstrAccesses = {
      {/* AccessSize */ 2u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false, /* NumElements */ 16, /* MinStride */ 1u},
      {/* AccessSize */ 4u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false, /* NumElements */ 16, /* MinStride */ 1u},
      {/* AccessSize */ 8u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false, /* NumElements */ 16, /* MinStride */ 1u},
      {/* AccessSize */ 8u, /* Alignment */ 8u, /* AllowMisalign */ false,
       /* BurstMode */ false, /* NumElements */ 16, /* MinStride */ 8u},
      {/* AccessSize */ 8u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false, /* NumElements */ 16, /* MinStride */ 3u},
      {/* AccessSize */ 7u, /* Alignment */ 1u, /* AllowMisalign */ true,
       /* BurstMode */ false, /* NumElements */ 13, /* MinStride */ 5u},
      {/* AccessSize */ 3u, /* Alignment */ 4u, /* AllowMisalign */ false,
       /* BurstMode */ false, /* NumElements */ 17, /* MinStride */ 7u},
  };

  auto TestCase = GetParam();
  auto &MR = TestCase.MR;

  RandEngine::init(::testing::GTEST_FLAG(random_seed));

  llvm::for_each(InstrAccesses, [&](const AddressGenInfo &GenInfo) {
    if (!MR.isLegal(GenInfo))
      return;
    for (unsigned I = 0; I < 1024; ++I) {
      auto Res = MR.randomAddress(GenInfo);
      for (size_t I = 0; I < GenInfo.NumElements; ++I) {
        assertValidAddress(Res, MR, GenInfo);
        Res.Address += Res.MinStride;
      }
    }
  });
}

INSTANTIATE_TEST_SUITE_P(
    MemoryRanges, MemoryRangesTest,
    ::testing::Values(
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 0u, /* LastOffset */ 0u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 1u, /* LastOffset */ 4u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 1u, /* LastOffset */ 4u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 4u,
                            /* FirstOffset */ 0u, /* LastOffset */ 2u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 0u, /* LastOffset */ 0u,
                            /* AccessSize */ 4u, /* MaxPastLastOffset */ 3u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 0u, /* LastOffset */ 1u,
                            /* AccessSize */ 4u, /* MaxPastLastOffset */ 2u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 4u,
                            /* FirstOffset */ 0u, /* LastOffset */ 1u,
                            /* AccessSize */ 4u, /* MaxPastLastOffset */ 3u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 14u,
                            /* FirstOffset */ 1u, /* LastOffset */ 5u,
                            /* AccessSize */ 4u, /* MaxPastLastOffset */ 2u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u,
                            /* Stride */ 1000u, /* FirstOffset */ 7u,
                            /* LastOffset */ 8u, /* AccessSize */ 8u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u,
                            /* Stride */ 1000u, /* FirstOffset */ 7u,
                            /* LastOffset */ 8u, /* AccessSize */ 100u,
                            /* MaxPastLastOffset */ 100u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u,
                            /* Stride */ 1000u, /* FirstOffset */ 3u,
                            /* LastOffset */ 8u, /* AccessSize */ 100u,
                            /* MaxPastLastOffset */ 4u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 2u, /* LastOffset */ 5u,
                            /* AccessSize */ 10u, /* MaxPastLastOffset */ 6u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 8u,
                            /* FirstOffset */ 3u, /* LastOffset */ 5u,
                            /* AccessSize */ 10u, /* MaxPastLastOffset */ 7u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 1024u, /* Stride */ 1u,
                            /* FirstOffset */ 0u, /* LastOffset */ 0u),
        MemoryRangeTestCase(/* Start */ 0u, /* Size */ 513u, /* Stride */ 7u,
                            /* FirstOffset */ 0u, /* LastOffset */ 0u)));
