// 控制面 × 内核集成剧本：签名指令经收件箱生效、采样策略落到轨迹通道、
// 策略参数落到管线调度、自愈总开关、回执回流为事件记录。
#include <unistd.h>

#include <string>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs/obs_kernel.h"
#include "imobs/policy.h"
#include "imobs/records.h"
#include "imobs_test.h"

using imobs::ActionSpec;
using imobs::Command;
using imobs::CommandType;
using imobs::EncodeCommand;
using imobs::EncodePolicyBundle;
using imobs::Event;
using imobs::EventKind;
using imobs::HealthOp;
using imobs::HealthState;
using imobs::IActionExecutor;
using imobs::IChannel;
using imobs::kEnvelopeCommand;
using imobs::kEnvelopePolicy;
using imobs::KernelCore;
using imobs::KernelOptions;
using imobs::PolicyBundle;
using imobs::Priority;
using imobs::SceneSpec;
using imobs::SchemaRegistry;
using imobs::SignEnvelope;
using imobs::VirtualClock;
using imobs::WireReader;

namespace {

constexpr const char* kKey = "test-only-control-key-not-for-production";  // Explicit test fixture, never a runtime default
constexpr uint64_t kSceneConn = 0xC0;
constexpr uint64_t kSigDead = 0x51;

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_kpol_XXXXXX";
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
  int executed = 0;
  void Execute(uint64_t, const std::string&, DoneFn done) override {
    ++executed;
    done(true);
  }
  void Verify(uint64_t, DoneFn done) override { done(true); }
};

KernelOptions BaseOpt() {
  KernelOptions o;
  o.control_key = kKey;
  o.ring_shards = 1;
  o.ring_capacity_per_shard = 1 << 8;
  o.trace_chunk_max_events = 4;
  o.pipeline.p1_flush_bytes = 1;
  for (auto& w : o.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  return o;
}

SceneSpec ConnScene() {
  SceneSpec s;
  s.scene_id = kSceneConn;
  s.detector.all_of = {kSigDead};
  s.detector.debounce_ms = 2'000;
  s.detector.debounce_jitter_ms = 0;
  s.detector.diagnose_ms = 1'000;
  ActionSpec a;
  a.name = "reconnect";
  a.timeout_ms = 3'000;
  s.ladder = {a};
  return s;
}

std::vector<uint8_t> SignedPolicy(const PolicyBundle& b) {
  std::vector<uint8_t> payload;
  EncodePolicyBundle(b, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopePolicy, payload, kKey, &env);
  return env;
}

std::vector<uint8_t> SignedCommand(const Command& c) {
  std::vector<uint8_t> payload;
  EncodeCommand(c, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopeCommand, payload, kKey, &env);
  return env;
}

Event TraceEv(VirtualClock& clk, uint64_t msg_id) {
  Event e{};
  e.ts_mono_ns = clk.MonoNanos();
  e.entity_id = msg_id;
  e.stage = 4;
  e.kind = static_cast<uint8_t>(EventKind::kTrace);
  return e;
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

// 汇集通道里全部 'E' 事件记录（收回执用）
struct SeenEvent {
  imobs::EventRecordWire rec;
  Priority pri;
};
std::vector<SeenEvent> CollectEvents(const std::vector<FakeChannel::SentBatch>& sent) {
  std::vector<SeenEvent> out;
  for (const auto& batch : sent) {
    WireReader r(batch.payload.data(), batch.payload.size());
    uint64_t id = 0, wall = 0, mono = 0, n = 0;
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
      if (len > 0 && p[0] == imobs::kRecEvent) {
        imobs::EventRecordWire rec;
        if (DecodeEventRecord(p, len, &rec)) out.push_back({rec, batch.pri});
      }
    }
  }
  return out;
}

bool HasTraceChunk(const std::vector<FakeChannel::SentBatch>& sent) {
  for (const auto& batch : sent) {
    WireReader r(batch.payload.data(), batch.payload.size());
    uint64_t id = 0, wall = 0, mono = 0, n = 0;
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
      if (len > 0 && p[0] == imobs::kRecTrace) return true;
    }
  }
  return false;
}

}  // namespace

TEST(KernelPolicy, SignedMuteCommandMutesSceneAndEmitsReceipt) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  DomainExecutor ex;
  auto opt = BaseOpt();
  opt.heal_scenes.push_back({ConnScene(), &ex});
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, opt, &err);
  ASSERT_TRUE(core != nullptr);

  Command mute;
  mute.command_id = 77;
  mute.type = CommandType::kMuteHealScene;
  mute.arg_a = kSceneConn;
  mute.arg_b = 600'000;
  const auto env = SignedCommand(mute);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();

  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kMuted);
  // 静默期内信号活跃也不动手
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kSignal, kSigDead, 1)));
  for (int t = 0; t < 5; ++t) {
    clk.AdvanceMillis(1'000);
    core->Step();
  }
  EXPECT_EQ(ex.executed, 0);

  // 回执以事件记录回流：key 固定、a=command_id、b=ok
  bool saw_receipt = false;
  for (const auto& e : CollectEvents(ch.sent)) {
    if (e.rec.key == imobs::kPolicyReceiptEventKey && e.rec.a == 77 && e.rec.b == 1) {
      saw_receipt = true;
      EXPECT_TRUE(e.pri == Priority::kP1);
    }
  }
  EXPECT_TRUE(saw_receipt);
}

TEST(KernelPolicy, TraceSamplingFollowsBundleAndForceOverride) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, BaseOpt(), &err);
  ASSERT_TRUE(core != nullptr);

  PolicyBundle b;
  b.version = 1;
  b.trace_sample_permille = 0;  // 应急全关轨迹
  b.rpc_trace_sample_permille = 0;
  const auto env = SignedPolicy(b);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();  // 策略生效

  for (uint64_t i = 1; i <= 4; ++i) ASSERT_TRUE(core->Emit(TraceEv(clk, 0x9000 + i)));
  core->Step();
  clk.AdvanceMillis(6'000);
  core->Step();  // 超块龄也不该有轨迹块——事件在采样口就被丢了（有账）
  EXPECT_FALSE(HasTraceChunk(ch.sent));
  EXPECT_EQ(core->self_stats().trace_sampled_out, 4u);

  // 故障排查：临时全采指令（TTL 强制）→ 轨迹恢复流动
  Command force;
  force.command_id = 88;
  force.type = CommandType::kForceTraceSample;
  force.arg_a = 1000;
  force.arg_b = 60'000;
  const auto fenv = SignedCommand(force);
  core->SubmitControlEnvelope(fenv.data(), fenv.size());
  core->Step();
  for (uint64_t i = 1; i <= 4; ++i) ASSERT_TRUE(core->Emit(TraceEv(clk, 0xA000 + i)));
  core->Step();  // 按量（4）冲刷 → P1 发出
  EXPECT_TRUE(HasTraceChunk(ch.sent));
}

TEST(KernelPolicy, FlushIntervalFromBundleDrivesP1Cadence) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  auto opt = BaseOpt();
  opt.pipeline.p1_flush_bytes = 1'000'000;  // 关掉按字节成批：只看时间窗
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, opt, &err);
  ASSERT_TRUE(core != nullptr);

  PolicyBundle b;
  b.version = 1;
  b.p1_flush_interval_ms = 5'000;  // 默认 30s → 策略压到 5s
  const auto env = SignedPolicy(b);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();

  Event ev{};
  ev.ts_mono_ns = clk.MonoNanos();
  ev.key = 0xE1;
  ev.stage = 1;  // P1 事件
  ev.kind = static_cast<uint8_t>(EventKind::kEvent);
  ASSERT_TRUE(core->Emit(ev));
  core->Step();
  EXPECT_EQ(ch.sent.size(), size_t{0});  // 未到 5s 窗口

  clk.AdvanceMillis(5'000);
  core->Step();  // 策略生效的节奏：5s 即发（默认 30s 此刻还发不出）
  EXPECT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP1);
}

TEST(KernelPolicy, HealerMasterSwitchStopsNewCures) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  DomainExecutor ex;
  auto opt = BaseOpt();
  opt.heal_scenes.push_back({ConnScene(), &ex});
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, opt, &err);
  ASSERT_TRUE(core != nullptr);

  PolicyBundle off;
  off.version = 1;
  off.healer_enabled = false;
  const auto env_off = SignedPolicy(off);
  core->SubmitControlEnvelope(env_off.data(), env_off.size());
  ASSERT_TRUE(core->Emit(HealthEv(clk, HealthOp::kSignal, kSigDead, 1)));
  for (int t = 0; t < 10; ++t) {
    clk.AdvanceMillis(1'000);
    core->Step();
  }
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kHealthy);  // 判定未启动
  EXPECT_EQ(ex.executed, 0);

  PolicyBundle on;
  on.version = 2;
  on.healer_enabled = true;
  const auto env_on = SignedPolicy(on);
  core->SubmitControlEnvelope(env_on.data(), env_on.size());
  core->Step();  // 信号仍活跃 → Suspect
  clk.AdvanceMillis(2'000);
  core->Step();
  clk.AdvanceMillis(1'000);
  core->Step();
  EXPECT_EQ(ex.executed, 1);
  EXPECT_TRUE(core->healer().state(kSceneConn) == HealthState::kHealthy);
}

// —— 回捞闭环：consent 门 → 内置执行体 → 'L' 分片上行 → 回执对账 ——
namespace {

struct SeenLogChunk {
  uint64_t command_id;
  std::vector<imobs::LogEntryWire> entries;
};
std::vector<SeenLogChunk> CollectLogChunks(const std::vector<FakeChannel::SentBatch>& sent) {
  std::vector<SeenLogChunk> out;
  for (const auto& batch : sent) {
    imobs::BatchView v;
    if (!imobs::ParseBatchView(batch.payload.data(), batch.payload.size(), &v)) continue;
    for (const auto& rec : v.records) {
      if (rec.empty() || static_cast<uint8_t>(rec[0]) != imobs::kRecLogs) continue;
      SeenLogChunk c;
      if (imobs::DecodeLogChunk(reinterpret_cast<const uint8_t*>(rec.data()), rec.size(),
                                &c.command_id, &c.entries)) {
        out.push_back(std::move(c));
      }
    }
  }
  return out;
}

}  // namespace

TEST(KernelPolicy, FetchLogsConsentGateThenChunkedUploadWithModuleFilter) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  auto opt = BaseOpt();
  const std::string dir = TmpDir();
  opt.log_path = dir + "/obs.log";
  std::string err;
  auto core = KernelCore::Open(dir, &clk, &ch, &reg, opt, &err);
  ASSERT_TRUE(core != nullptr);

  // 记两条日志（不同模块），Step 落盘。上行通道此刻不该有任何 'L' 记录——
  // "全都记、都不传"。
  const uint64_t mod_link = imobs::Fnv1a("imlink");
  const uint64_t mod_db = imobs::Fnv1a("imdb");
  core->Log(2, mod_link, "conn reset uid=42");
  clk.AdvanceMillis(10);
  core->Log(3, mod_db, "sql slow 812ms");
  core->Step();
  EXPECT_EQ(CollectLogChunks(ch.sent).size(), size_t{0});

  Command fetch;
  fetch.command_id = 91;
  fetch.type = CommandType::kFetchLogs;
  fetch.arg_a = 0;
  fetch.arg_b = static_cast<uint64_t>(clk.WallMillis());

  // consent 关（默认）：执行失败、回执 ok=0、通道上没有 'L'。
  auto env = SignedCommand(fetch);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();
  EXPECT_EQ(CollectLogChunks(ch.sent).size(), size_t{0});
  bool saw_denied = false;
  for (const auto& e : CollectEvents(ch.sent)) {
    if (e.rec.key == imobs::kPolicyReceiptEventKey && e.rec.a == 91 && e.rec.b == 0) {
      saw_denied = true;
    }
  }
  EXPECT_TRUE(saw_denied);

  // consent 开：新命令 id（幂等窗口）→ 两条全部上行 + 回执 ok=1。
  core->SetFetchConsent(true);
  fetch.command_id = 92;
  env = SignedCommand(fetch);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();
  auto chunks = CollectLogChunks(ch.sent);
  ASSERT_EQ(chunks.size(), size_t{1});
  EXPECT_EQ(chunks[0].command_id, 92u);
  ASSERT_EQ(chunks[0].entries.size(), size_t{2});
  EXPECT_TRUE(chunks[0].entries[0].text == "conn reset uid=42");
  EXPECT_EQ(chunks[0].entries[0].module_hash, mod_link);
  EXPECT_TRUE(chunks[0].entries[1].text == "sql slow 812ms");

  // 模块过滤：arg_s="imdb" → 只回 db 那条。
  fetch.command_id = 93;
  fetch.arg_s = "imdb";
  env = SignedCommand(fetch);
  core->SubmitControlEnvelope(env.data(), env.size());
  core->Step();
  chunks = CollectLogChunks(ch.sent);
  ASSERT_EQ(chunks.size(), size_t{2});
  ASSERT_EQ(chunks[1].entries.size(), size_t{1});
  EXPECT_EQ(chunks[1].entries[0].module_hash, mod_db);
  core->Shutdown();
}

IMOBS_TEST_MAIN
