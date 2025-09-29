#include "snippy/Config/MemoryScheme.h"

#include "gtest/gtest.h"

using namespace llvm::snippy;

static bool IsValidAddress(AddressInfo Res, const MemoryAccessRange &MR) {
  auto Start = MR.Start;
  auto Size = MR.Size;
  auto Stride = MR.Stride;
  auto FirstOff = MR.FirstOffset;
  auto LastOff = MR.LastOffset;
  auto MaxAccSize = MR.AccessSize;
  auto MaxOffset = MR.MaxPastLastOffset;

  auto Addr = Res.Address;
  auto Offset = (Addr - Start) % Stride;
  auto AISize = Res.AccessSize;

  bool Cond = Offset >= FirstOff && Offset <= LastOff;
  Cond &= (Addr >= Start) && (Addr + AISize < Start + Size);

  if (MaxAccSize)
    Cond &= AISize <= *MaxAccSize;
  if (MaxOffset)
    Cond &= (Offset + AISize) <= (LastOff + 1 + *MaxOffset);
  return Cond;
}

struct MemoryRangeTestCase {
  MemoryAccessRange MR;

  template <typename... MRArgs>
  MemoryRangeTestCase(MRArgs... Args) : MR(Args...){};
};

class MemoryRangesTest : public ::testing::TestWithParam<MemoryRangeTestCase> {
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
  llvm::for_each(InstrAccesses, [&](const auto &Access) {
    const auto &Res = MR.randomAddress(Access);
    EXPECT_PRED2(IsValidAddress, Res, MR);
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
                            /* AccessSize */ 10u, /* MaxPastLastOffset */ 7u)));
