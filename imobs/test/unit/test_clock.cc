#include "imobs/clock.h"

#include "imobs_test.h"

TEST(Clock, VirtualAdvance) {
  imobs::VirtualClock c(0, 1000);
  EXPECT_EQ(c.MonoNanos(), 0u);
  c.AdvanceMillis(250);
  EXPECT_EQ(c.MonoNanos(), 250'000'000ull);
  EXPECT_EQ(c.WallMillis(), 1250);
  c.AdvanceNanos(24ull * 3600 * 1'000'000'000);  // 快进 24h：冷却期测试的地基
  EXPECT_EQ(c.WallMillis(), 1250 + 24 * 3600 * 1000);
}

TEST(Clock, AnchorConversion) {
  // 锚点 (wall=1000ms, mono=5s)：mono=6s → wall=2000ms；反向偏移同样成立
  const imobs::ClockAnchor a{1000, 5'000'000'000ull};
  EXPECT_EQ(imobs::WallFromMono(a, 6'000'000'000ull), 2000);
  EXPECT_EQ(imobs::WallFromMono(a, 4'500'000'000ull), 500);
}

TEST(Clock, RealMonotoneNonDecreasing) {
  auto& c = imobs::RealClock::Instance();
  uint64_t prev = c.MonoNanos();
  for (int i = 0; i < 10'000; ++i) {
    const uint64_t now = c.MonoNanos();
    ASSERT_TRUE(now >= prev);
    prev = now;
  }
}

IMOBS_TEST_MAIN
