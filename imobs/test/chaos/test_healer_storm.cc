// 自愈风暴仿真（实施计划 P4 决策门）：
// 1000 实例 × 全局故障 30s → 全程零自愈动作（"大家都坏"时只等待）；
// 故障解除后靠防抖抖动把确诊/执行时刻摊开——重连风暴的分布证明。
#include <algorithm>
#include <map>
#include <vector>

#include "imobs/clock.h"
#include "imobs/healer.h"
#include "imobs_test.h"

using imobs::ActionSpec;
using imobs::DamageLevel;
using imobs::HealEvent;
using imobs::Healer;
using imobs::HealerOptions;
using imobs::HealthState;
using imobs::IActionExecutor;
using imobs::SceneSpec;
using imobs::VirtualClock;

namespace {

constexpr uint64_t kSigBase = 0xD00D0000;

// 建模要点：修复动作成功即消除该实例的根因信号（重连成功 = "连接死"信号消失）。
// 若信号无人清除，治愈的实例会立刻复发重治——那测的就不是风暴而是热循环。
struct StormExecutor final : IActionExecutor {
  StormExecutor(imobs::IClock* c, Healer* h) : clk(c), healer(h) {}
  imobs::IClock* clk;
  Healer* healer;
  std::vector<uint64_t> exec_times_ms;
  void Execute(uint64_t scene_id, const std::string&, DoneFn done) override {
    exec_times_ms.push_back(clk->MonoNanos() / 1'000'000ull);
    healer->SetSignal(kSigBase + scene_id, false);  // 治愈 = 根因信号消除
    done(true);
  }
  void Verify(uint64_t, DoneFn done) override { done(true); }
};

}  // namespace

TEST(HealerStorm, GlobalOutageHoldsThenJitterSpreadsRecovery) {
  VirtualClock clk;
  uint64_t events = 0;
  HealerOptions opt;
  opt.rng_seed = 99;
  Healer h(&clk, [&](const HealEvent&) { ++events; }, opt);
  StormExecutor ex(&clk, &h);

  constexpr int kN = 1000;
  for (int i = 1; i <= kN; ++i) {
    SceneSpec s;
    s.scene_id = static_cast<uint64_t>(i);
    s.detector.all_of = {kSigBase + static_cast<uint64_t>(i)};  // 每实例独立根因信号
    s.detector.debounce_ms = 2'000;
    s.detector.debounce_jitter_ms = 3'000;  // 风暴防线：确诊时刻摊开在 3s 抖动窗内
    s.detector.diagnose_ms = 500;
    ActionSpec a;
    a.name = "reconnect";
    a.level = DamageLevel::kL0;
    a.cooldown_ms = 0;
    a.timeout_ms = 5'000;
    s.ladder = {a};
    ASSERT_TRUE(h.RegisterScene(s, &ex));
  }

  // —— 阶段一：全局故障 30s，全部实例根因信号活跃 ——
  for (int i = 1; i <= kN; ++i) h.SetSignal(kSigBase + static_cast<uint64_t>(i), true);
  h.SetGlobalOutage(true);
  for (int t = 0; t < 300; ++t) {
    clk.AdvanceMillis(100);
    h.Tick();
  }
  EXPECT_EQ(ex.exec_times_ms.size(), size_t{0});  // 决策门：故障期间零齐步重连
  EXPECT_EQ(h.stats().heal_attempts, 0u);

  // —— 阶段二：故障解除，恢复期 10s ——
  h.SetGlobalOutage(false);
  const uint64_t t0 = clk.MonoNanos() / 1'000'000ull;
  for (int t = 0; t < 200; ++t) {
    clk.AdvanceMillis(50);
    h.Tick();
  }
  ASSERT_EQ(ex.exec_times_ms.size(), static_cast<size_t>(kN));  // 全部自愈发生
  EXPECT_EQ(h.stats().heal_success, static_cast<uint64_t>(kN));

  // 分布断言：任一 500ms 桶的执行占比 < 30%（无抖动时全部同桶 = 100%）
  std::map<uint64_t, int> buckets;
  uint64_t earliest = UINT64_MAX;
  for (const uint64_t ts : ex.exec_times_ms) {
    ++buckets[(ts - t0) / 500];
    earliest = std::min(earliest, ts - t0);
  }
  int max_bucket = 0;
  for (const auto& [b, n] : buckets) {
    (void)b;
    max_bucket = std::max(max_bucket, n);
  }
  EXPECT_TRUE(max_bucket < kN * 30 / 100);
  EXPECT_TRUE(buckets.size() >= 5);        // 执行时刻确实被摊开成多个时间片
  EXPECT_TRUE(earliest >= 2'400);          // 下限 = 防抖 2s + 交叉验证 0.5s（含 tick 粒度）
  EXPECT_EQ(h.stats().false_positives, 0u);
  EXPECT_EQ(h.stats().unexpected_transitions, 0u);
  EXPECT_TRUE(events >= static_cast<uint64_t>(kN) * 5);  // 全量上报：每实例至少五次迁移留痕
}

IMOBS_TEST_MAIN
