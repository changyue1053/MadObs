// 自愈框架确定性剧本（实施计划 P4.1~P4.4 DoD）：虚拟时钟驱动，毫秒级复现。
#include "imobs/healer.h"

#include <map>
#include <string>
#include <vector>

#include "imobs/clock.h"
#include "imobs_test.h"

using imobs::ActionSpec;
using imobs::DamageLevel;
using imobs::DetectorSpec;
using imobs::HealEvent;
using imobs::Healer;
using imobs::HealerOptions;
using imobs::HealReason;
using imobs::HealthState;
using imobs::IActionExecutor;
using imobs::SceneSpec;
using imobs::VirtualClock;

namespace {

constexpr uint64_t kSig = 0x51;

struct ScriptedExecutor final : IActionExecutor {
  explicit ScriptedExecutor(imobs::IClock* c) : clk(c) {}
  imobs::IClock* clk;
  std::map<std::string, std::vector<bool>> exec_script;  // 动作名 → 结果队列（空=成功）
  std::vector<bool> verify_script;                       // 校验结果队列（空=成功）
  std::vector<std::string> async_actions;                // 挂起 done 的动作（超时/静默剧本）
  std::vector<DoneFn> pending;
  std::vector<std::string> executed;
  int verify_calls = 0;

  bool IsAsync(const std::string& a) const {
    for (const auto& x : async_actions) {
      if (x == a) return true;
    }
    return false;
  }
  void Execute(uint64_t, const std::string& a, DoneFn done) override {
    executed.push_back(a);
    if (IsAsync(a)) {
      pending.push_back(std::move(done));
      return;
    }
    bool ok = true;
    auto it = exec_script.find(a);
    if (it != exec_script.end() && !it->second.empty()) {
      ok = it->second.front();
      it->second.erase(it->second.begin());
    }
    done(ok);
  }
  void Verify(uint64_t, DoneFn done) override {
    ++verify_calls;
    bool ok = true;
    if (!verify_script.empty()) {
      ok = verify_script.front();
      verify_script.erase(verify_script.begin());
    }
    done(ok);
  }
};

ActionSpec A(std::string name, DamageLevel lv = DamageLevel::kL0, uint64_t cooldown = 0,
             uint64_t timeout = 3'000, uint32_t per_day = 100, bool pre = false) {
  ActionSpec a;
  a.name = std::move(name);
  a.level = lv;
  a.cooldown_ms = cooldown;
  a.timeout_ms = timeout;
  a.max_per_day = per_day;
  a.needs_precondition = pre;
  return a;
}

SceneSpec MakeScene(uint64_t id, std::vector<ActionSpec> ladder, uint64_t sig = kSig) {
  SceneSpec s;
  s.scene_id = id;
  s.detector.all_of = {sig};
  s.detector.debounce_ms = 2'000;
  s.detector.debounce_jitter_ms = 0;  // 单元剧本免抖动；抖动由风暴仿真覆盖
  s.detector.diagnose_ms = 1'000;
  s.ladder = std::move(ladder);
  s.verify_timeout_ms = 5'000;
  s.breaker_threshold = 3;
  s.muted_ttl_ms = 60'000;
  return s;
}

// 信号已活跃的前提下，走完 防抖 → 交叉验证 → 确诊 的标准节拍
void DriveToConfirm(Healer& h, VirtualClock& clk) {
  h.Tick();  // Healthy → Suspect
  clk.AdvanceMillis(2'000);
  h.Tick();  // → Diagnosing
  clk.AdvanceMillis(1'000);
  h.Tick();  // 确诊 → 升级链
}

}  // namespace

TEST(Healer, HappyPathHealAtFirstLevel) {
  VirtualClock clk;
  std::vector<HealEvent> evs;
  Healer h(&clk, [&](const HealEvent& e) { evs.push_back(e); });
  ScriptedExecutor ex(&clk);
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("reconnect")}), &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);

  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  const auto st = h.stats();
  EXPECT_EQ(st.heal_attempts, 1u);
  EXPECT_EQ(st.heal_success, 1u);
  EXPECT_EQ(st.escalations, 0u);
  ASSERT_EQ(ex.executed.size(), size_t{1});
  EXPECT_TRUE(ex.executed[0] == "reconnect");
  EXPECT_EQ(ex.verify_calls, 1);
  // 全量上报铁律：五次迁移全部留痕，reason 链完整
  ASSERT_EQ(evs.size(), size_t{5});
  EXPECT_TRUE(evs[0].reason == HealReason::kSignal);
  EXPECT_TRUE(evs[1].reason == HealReason::kDebouncePassed);
  EXPECT_TRUE(evs[2].reason == HealReason::kConfirmed);
  EXPECT_TRUE(evs[3].reason == HealReason::kActionOk);
  EXPECT_TRUE(evs[4].reason == HealReason::kVerifyOk);
  EXPECT_EQ(h.stats().unexpected_transitions, 0u);
}

TEST(Healer, DebounceFilterAndFalsePositive) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("fix")}), &ex));

  // 防抖窗口内信号消退：正常过滤，不算误报
  h.SetSignal(kSig, true);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kSuspect);
  h.SetSignal(kSig, false);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().false_positives, 0u);

  // 交叉验证窗口内信号消退：误报，记账
  h.SetSignal(kSig, true);
  h.Tick();
  clk.AdvanceMillis(2'000);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kDiagnosing);
  h.SetSignal(kSig, false);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().false_positives, 1u);
  EXPECT_EQ(ex.executed.size(), size_t{0});  // 全程没动过手术刀
  EXPECT_EQ(h.stats().heal_attempts, 0u);
}

TEST(Healer, EscalatesThroughLadderUntilVerified) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ex.exec_script["a1"] = {false};      // a1 执行失败
  ex.verify_script = {false, true};    // a2 校验失败、a3 校验通过
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("a1"), A("a2"), A("a3")}), &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);

  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  ASSERT_EQ(ex.executed.size(), size_t{3});
  EXPECT_TRUE(ex.executed[0] == "a1");
  EXPECT_TRUE(ex.executed[1] == "a2");
  EXPECT_TRUE(ex.executed[2] == "a3");
  const auto st = h.stats();
  EXPECT_EQ(st.escalations, 2u);   // a1 执行失败 + a2 校验失败
  EXPECT_EQ(st.heal_attempts, 3u);
  EXPECT_EQ(st.heal_success, 1u);
  EXPECT_EQ(ex.verify_calls, 2);
}

TEST(Healer, CooldownSkipsLevelOnRelapse) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ASSERT_TRUE(h.RegisterScene(
      MakeScene(1, {A("light", DamageLevel::kL0, /*cooldown=*/100'000), A("stronger")}), &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);  // 首愈走 light
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);

  h.SetSignal(kSig, false);
  h.Tick();
  h.SetSignal(kSig, true);  // 复发（仍在 light 冷却期内）
  DriveToConfirm(h, clk);

  ASSERT_EQ(ex.executed.size(), size_t{2});
  EXPECT_TRUE(ex.executed[0] == "light");
  EXPECT_TRUE(ex.executed[1] == "stronger");  // 冷却期内跳过 light，直上一级
  EXPECT_EQ(h.stats().heal_success, 2u);
}

TEST(Healer, ExhaustedThreeTimesTripsBreakerThenTtlRecovers) {
  VirtualClock clk;
  std::vector<HealEvent> evs;
  Healer h(&clk, [&](const HealEvent& e) { evs.push_back(e); });
  ScriptedExecutor ex(&clk);
  ex.exec_script["only"] = {false, false, false};  // 三个疗程全失败
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("only")}), &ex));

  h.SetSignal(kSig, true);  // 信号持续活跃（根因未除）
  for (int cycle = 0; cycle < 3; ++cycle) DriveToConfirm(h, clk);

  const auto st = h.stats();
  EXPECT_EQ(st.exhausted, 3u);
  EXPECT_EQ(st.breaker_trips, 1u);
  EXPECT_TRUE(h.state(1) == HealthState::kMuted);  // 连续力竭 → 本地熔断

  // 熔断期内：信号活跃也绝不动手
  clk.AdvanceMillis(10'000);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kMuted);
  EXPECT_EQ(ex.executed.size(), size_t{3});

  clk.AdvanceMillis(50'000);  // TTL 60s 到期
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  bool saw_breaker = false;
  for (const auto& e : evs) {
    if (e.reason == HealReason::kBreakerTrip) saw_breaker = true;
  }
  EXPECT_TRUE(saw_breaker);
}

TEST(Healer, GlobalOutageFreezesDetection) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("reconnect")}), &ex));

  // "大家都坏"：30 秒内信号持续活跃，判定全程冻结
  h.SetGlobalOutage(true);
  h.SetSignal(kSig, true);
  for (int t = 0; t < 30; ++t) {
    clk.AdvanceMillis(1'000);
    h.Tick();
  }
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(ex.executed.size(), size_t{0});

  // 故障中途置位：已进 Suspect 的场景退回并记账
  h.SetGlobalOutage(false);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kSuspect);
  h.SetGlobalOutage(true);
  h.Tick();
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().suppressed_by_global, 1u);

  // 全局故障解除后正常自愈
  h.SetGlobalOutage(false);
  DriveToConfirm(h, clk);
  EXPECT_EQ(h.stats().heal_success, 1u);
}

TEST(Healer, KillSwitchMutesAndIgnoresStaleCallback) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ex.async_actions = {"slow"};  // 执行挂起：模拟域动作在途
  ASSERT_TRUE(h.RegisterScene(MakeScene(1, {A("slow")}), &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);
  EXPECT_TRUE(h.state(1) == HealthState::kHealing);
  ASSERT_EQ(ex.pending.size(), size_t{1});

  h.MuteScene(1, 100'000);  // 云端 kill switch：静默即终结当前疗程
  EXPECT_TRUE(h.state(1) == HealthState::kMuted);

  ex.pending[0](true); h.Tick();  // 迟到回执：epoch 已作废，必须被忽略
  EXPECT_TRUE(h.state(1) == HealthState::kMuted);
  EXPECT_EQ(ex.verify_calls, 0);

  h.UnmuteScene(1);
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
}

TEST(Healer, ExecTimeoutEscalatesAndLateDoneIgnored) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ex.async_actions = {"hang"};  // 永不回执
  ASSERT_TRUE(
      h.RegisterScene(MakeScene(1, {A("hang", DamageLevel::kL0, 0, /*timeout=*/3'000), A("b")}),
                      &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);
  EXPECT_TRUE(h.state(1) == HealthState::kHealing);

  clk.AdvanceMillis(3'000);
  h.Tick();  // 执行超时 → 升级到 b → 同步成功 → 校验通过 → 痊愈
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().escalations, 1u);
  ASSERT_EQ(ex.executed.size(), size_t{2});
  EXPECT_TRUE(ex.executed[1] == "b");

  ASSERT_EQ(ex.pending.size(), size_t{1});
  h.SetSignal(kSig, false);
  ex.pending[0](true); h.Tick();  // hang 的迟到回执：忽略，状态与计数不动
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().heal_success, 1u);
  EXPECT_EQ(ex.verify_calls, 1);
}

TEST(Healer, HeavyMutexAndPreconditionGateL2) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ex.async_actions = {"rebuild"};  // X 的重手术在途，占住全局互斥
  constexpr uint64_t kSigX = 0xA1;
  constexpr uint64_t kSigY = 0xA2;
  constexpr uint64_t kSigZ = 0xA3;
  ASSERT_TRUE(h.RegisterScene(
      MakeScene(1, {A("rebuild", DamageLevel::kL2, 0, 100'000, 100, /*pre=*/true)}, kSigX), &ex));
  ASSERT_TRUE(h.RegisterScene(
      MakeScene(2, {A("rebuild", DamageLevel::kL2, 0, 100'000, 100, /*pre=*/true)}, kSigY), &ex));
  h.SetPrecondition(1, true);
  h.SetPrecondition(2, true);

  h.SetSignal(kSigX, true);
  h.SetSignal(kSigY, true);
  DriveToConfirm(h, clk);  // 同刻确诊：X 先执行（占互斥），Y 被跳过 → 力竭
  ASSERT_EQ(ex.executed.size(), size_t{1});  // 同刻只允许一台重手术
  EXPECT_TRUE(h.state(1) == HealthState::kHealing);
  EXPECT_TRUE(h.state(2) == HealthState::kHealthy);  // Y 力竭后回到再检测
  EXPECT_EQ(h.stats().exhausted, 1u);

  h.SetSignal(kSigX, false);
  ex.pending[0](true); h.Tick();  // X 完成 → 校验通过 → 互斥释放
  EXPECT_TRUE(h.state(1) == HealthState::kHealthy);
  EXPECT_EQ(h.stats().heal_success, 1u);

  // 前置条件未就绪的 L2：确诊后直接力竭，绝不动手
  h.SetSignal(kSigX, false);
  h.SetSignal(kSigY, false);
  ASSERT_TRUE(h.RegisterScene(
      MakeScene(3, {A("rebuild3", DamageLevel::kL2, 0, 100'000, 100, /*pre=*/true)}, kSigZ), &ex));
  h.SetSignal(kSigZ, true);
  DriveToConfirm(h, clk);
  EXPECT_EQ(ex.executed.size(), size_t{1});  // rebuild3 未执行
  EXPECT_EQ(h.stats().exhausted, 2u);
}

TEST(Healer, DayQuotaLimitsAndResetsNextDay) {
  VirtualClock clk;
  Healer h(&clk, nullptr);
  ScriptedExecutor ex(&clk);
  ASSERT_TRUE(h.RegisterScene(
      MakeScene(1, {A("daily", DamageLevel::kL0, 0, 3'000, /*per_day=*/1)}, kSig), &ex));

  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);  // 当日第 1 次：放行
  EXPECT_EQ(h.stats().heal_success, 1u);

  h.SetSignal(kSig, false);
  h.Tick();
  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);  // 当日第 2 次：频控扣住 → 力竭
  EXPECT_EQ(ex.executed.size(), size_t{1});
  EXPECT_EQ(h.stats().exhausted, 1u);

  clk.AdvanceMillis(86'400'000);  // 次日预算重置
  h.SetSignal(kSig, false);
  h.Tick();
  h.SetSignal(kSig, true);
  DriveToConfirm(h, clk);
  EXPECT_EQ(ex.executed.size(), size_t{2});
  EXPECT_EQ(h.stats().heal_success, 2u);
}

IMOBS_TEST_MAIN
