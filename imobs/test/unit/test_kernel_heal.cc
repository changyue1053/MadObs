// 自愈域 × 内核集成剧本（实施计划 P4.5）：MAPE 环在内核内闭合——
// 域信号经分片环跨线程进入健康域 → Step 内 Tick 驱动升级链 → 自愈事件
// 按级别回流上报管线（力竭/熔断/静默 = P0），"自愈全程留痕"落到通道字节。
#include <unistd.h>

#include <string>
#include <vector>

#include "imobs/healer.h"
#include "imobs/obs_kernel.h"
#include "imobs/records.h"
#include "imobs_test.h"

using imobs::ActionSpec;
using imobs::DamageLevel;
using imobs::Event;
using imobs::EventKind;
using imobs::HealRecordWire;
using imobs::HealReason;
using imobs::HealthOp;
using imobs::HealthState;
using imobs::IActionExecutor;
using imobs::IChannel;
using imobs::KernelCore;
using imobs::KernelOptions;
using imobs::Priority;
using imobs::SceneSpec;
using imobs::SchemaRegistry;
using imobs::VirtualClock;
using imobs::WireReader;

namespace {

constexpr uint64_t kSceneConn = 0xC0;
constexpr uint64_t kSigDead = 0x51;

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_kheal_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

struct FakeChannel final : IChannel {
  struct SentBatch {
    Priority pri;
    uint64_t id;
    std::string payload;
  };
  std::vector<SentBatch> sent;
  void Send(Priority pri, uint64_t id, std::string_view payload, SendCallback cb) override {
    sent.push_back({pri, id, std::string(payload)});
    cb({true});
  }
};

struct DomainExecutor final : IActionExecutor {
  std::vector<std::string> executed;
  bool exec_ok = true;
  void Execute(uint64_t, const std::string& a, DoneFn done) override {
    executed.push_back(a);
    done(exec_ok);
  }
  void Verify(uint64_t, DoneFn done) override { done(true); }
};

SceneSpec ConnScene(std::vector<ActionSpec> ladder) {
  SceneSpec s;
  s.scene_id = kSceneConn;
  s.detector.all_of = {kSigDead};
  s.detector.debounce_ms = 2'000;
  s.detector.debounce_jitter_ms = 0;
  s.detector.diagnose_ms = 1'000;
  s.ladder = std::move(ladder);
  s.verify_timeout_ms = 5'000;
  s.breaker_threshold = 1;  // 首次力竭即熔断（P0 路由断言用）
  s.muted_ttl_ms = 60'000;
  return s;
}

KernelOptions KOpt(IActionExecutor* exec, std::vector<ActionSpec> ladder) {
  KernelOptions o;
  o.ring_shards = 1;
  o.ring_capacity_per_shard = 1 << 8;
  o.pipeline.p1_flush_bytes = 1;  // 有货即成批
  for (auto& w : o.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  o.heal_scenes.push_back({ConnScene(std::move(ladder)), exec});
  return o;
}

Event HealthEv(VirtualClock& clk, HealthOp op, uint64_t key, uint64_t value) {
  Event e{};
  e.ts_mono_ns = clk.MonoNanos();
  e.key = key;
  e.value = value;
  e.stage = static_cast<uint8_t>(op);
  e.kind = static_cast<uint8_t>(EventKind::kHealth);
  return e;
}

// 汇集通道里全部 'H' 记录（保持到达顺序），并记录所属批的优先级
struct HealTrace {
  HealRecordWire rec;
  Priority pri;
};
std::vector<HealTrace> CollectHeals(const std::vector<FakeChannel::SentBatch>& sent) {
  std::vector<HealTrace> out;
  for (const auto& batch : sent) {
    WireReader r(batch.payload.data(), batch.payload.size());
    uint64_t id = 0;
    uint64_t wall = 0;
    uint64_t mono = 0;
    uint64_t n = 0;
    const uint8_t* magic = nullptr;
    uint8_t pri = 0;
    if (!r.Fixed64(&id) || !r.Bytes(&magic, 4) || !r.U8(&pri) || !r.Fixed64(&wall) ||
        !r.Fixed64(&mono) || !r.Varint(&n)) {
      continue;
    }
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t len = 0;
      const uint8_t* p = nullptr;
      if (!r.Varint(&len) || !r.Bytes(&p, len)) break;
      if (len > 0 && p[0] == imobs::kRecHeal) {
        HealRecordWire rec;
        if (DecodeHealRecord(p, len, &rec)) out.push_back({rec, batch.pri});
      }
    }
  }
  return out;
}

}  // namespace

TEST(KernelHeal, SignalThroughRingHealsAndLeavesFullAuditTrail) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  DomainExecutor ex;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(&ex, {[] {
                                 ActionSpec a;
                                 a.name = "reconnect";
                                 a.timeout_ms = 3'000;
                                 return a;
                               }()}),
                               &err);
  ASSERT_TRUE(core != nullptr);

  // 域信号跨线程语义：经环进入健康域（此处同线程 Emit，语义一致）
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kSignal, kSigDead, 1)));
  core->Step();  // 信号入域 + Tick → Suspect
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kSuspect);

  clk.AdvanceMillis(2'000);
  core->Step();  // → Diagnosing
  clk.AdvanceMillis(1'000);
  core->Step();  // 确诊 → 执行 → 校验 → 痊愈，事件已随本 Step 的 Poll 出通道

  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kHealthy);
  ASSERT_EQ(ex.executed.size(), size_t{1});

  const auto heals = CollectHeals(ch.sent);
  ASSERT_EQ(heals.size(), size_t{5});  // 全量留痕：五次迁移一条不少
  EXPECT_TRUE(heals[0].rec.reason == static_cast<uint8_t>(HealReason::kSignal));
  EXPECT_TRUE(heals[2].rec.reason == static_cast<uint8_t>(HealReason::kConfirmed));
  EXPECT_TRUE(heals[2].rec.action == "reconnect");
  EXPECT_TRUE(heals[4].rec.reason == static_cast<uint8_t>(HealReason::kVerifyOk));
  for (const auto& h : heals) {
    EXPECT_TRUE(h.pri == Priority::kP1);  // 常规疗程事件走 P1
    EXPECT_EQ(h.rec.scene_id, kSceneConn);
  }
}

TEST(KernelHeal, ExhaustAndBreakerRouteToP0) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  DomainExecutor ex;
  ex.exec_ok = false;  // 动作全失败 → 单级链力竭 → breaker_threshold=1 直接熔断
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(&ex, {[] {
                                 ActionSpec a;
                                 a.name = "only";
                                 a.timeout_ms = 3'000;
                                 return a;
                               }()}),
                               &err);
  ASSERT_TRUE(core != nullptr);

  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kSignal, kSigDead, 1)));
  core->Step();
  clk.AdvanceMillis(2'000);
  core->Step();
  clk.AdvanceMillis(1'000);
  core->Step();
  core->Step();  // 力竭与熔断是两条 P0 记录 = 两个批；分区内串行，一个 Poll 发一批

  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kMuted);
  bool saw_exhausted_p0 = false;
  bool saw_breaker_p0 = false;
  for (const auto& h : CollectHeals(ch.sent)) {
    if (h.rec.reason == static_cast<uint8_t>(HealReason::kExhaustedLadder) &&
        h.pri == Priority::kP0) {
      saw_exhausted_p0 = true;
    }
    if (h.rec.reason == static_cast<uint8_t>(HealReason::kBreakerTrip) &&
        h.pri == Priority::kP0) {
      saw_breaker_p0 = true;
    }
  }
  EXPECT_TRUE(saw_exhausted_p0);  // 力竭是体系级强信号：必须走 P0 即时通道
  EXPECT_TRUE(saw_breaker_p0);
}

TEST(KernelHeal, OutageAndKillSwitchOpsFlowThroughRing) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  DomainExecutor ex;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(&ex, {[] {
                                 ActionSpec a;
                                 a.name = "reconnect";
                                 a.timeout_ms = 3'000;
                                 return a;
                               }()}),
                               &err);
  ASSERT_TRUE(core != nullptr);

  // 全局故障位经环生效：判定冻结，域信号活跃也不动手
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kGlobalOutage, 0, 1)));
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kSignal, kSigDead, 1)));
  for (int t = 0; t < 10; ++t) {
    clk.AdvanceMillis(1'000);
    core->Step();
  }
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kHealthy);
  EXPECT_EQ(ex.executed.size(), size_t{0});

  // 云端 kill switch 经环生效：解除全局故障后场景仍被静默
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kGlobalOutage, 0, 0)));
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kMute, kSceneConn, 100'000)));
  core->Step();
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kMuted);
  for (int t = 0; t < 10; ++t) {
    clk.AdvanceMillis(1'000);
    core->Step();
  }
  EXPECT_EQ(ex.executed.size(), size_t{0});
  // 静默事件本身以 P0 留痕
  bool saw_kill_p0 = false;
  for (const auto& h : CollectHeals(ch.sent)) {
    if (h.rec.reason == static_cast<uint8_t>(HealReason::kKillSwitch) &&
        h.pri == Priority::kP0) {
      saw_kill_p0 = true;
    }
  }
  EXPECT_TRUE(saw_kill_p0);

  // 解除静默 → 正常自愈
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kUnmute, kSceneConn, 0)));
  core->Step();
  clk.AdvanceMillis(1);
  core->Step();  // 信号仍活跃 → Suspect
  clk.AdvanceMillis(2'000);
  core->Step();
  clk.AdvanceMillis(1'000);
  core->Step();
  EXPECT_EQ(ex.executed.size(), size_t{1});
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kHealthy);
}

IMOBS_TEST_MAIN
