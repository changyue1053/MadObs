// 内核装配端到端剧本（实施计划 P1.1 DoD）：
// 打点（分片环）→ 消费步进（聚合/轨迹块/事件分发）→ 窗口职责 → 管线 → 通道。
// KernelCore 用虚拟时钟全确定性验证；最后一个用例用真实线程 + 真实时钟冒烟。
#include "imobs/obs_kernel.h"

#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs/records.h"
#include "imobs_test.h"

using imobs::Event;
using imobs::EventKind;
using imobs::IChannel;
using imobs::KernelCore;
using imobs::KernelOptions;
using imobs::LabelDef;
using imobs::ObsKernel;
using imobs::Priority;
using imobs::SchemaRegistry;
using imobs::VirtualClock;
using imobs::WireReader;

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_kern_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

struct FakeChannel final : IChannel {
  struct SentBatch {
    Priority pri;
    uint64_t id;
    std::string payload;
  };
  std::vector<SentBatch> sent;  // 仅消费线程写；主线程在 Stop/join 之后读
  void Send(Priority pri, uint64_t id, std::string_view payload, SendCallback cb) override {
    sent.push_back({pri, id, std::string(payload)});
    cb({true});
  }
};

KernelOptions KOpt() {
  KernelOptions o;
  o.ring_shards = 4;
  o.ring_capacity_per_shard = 1 << 10;
  o.metrics_snapshot_interval_ms = 30'000;
  o.trace_chunk_max_events = 4;
  o.trace_chunk_max_age_ms = 5'000;
  o.pipeline.p1_flush_bytes = 1;         // 有货即成批（测试免等 30s 窗口）
  o.pipeline.p2_flush_interval_ms = 1;   // 时间一动即可成批
  for (auto& w : o.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  return o;
}

Event MetricEv(VirtualClock& clk, uint64_t key, uint16_t label, uint64_t v) {
  Event e{};
  e.ts_mono_ns = clk.MonoNanos();
  e.key = key;
  e.label_idx = label;
  e.value = v;
  e.kind = static_cast<uint8_t>(EventKind::kMetric);
  return e;
}

Event TraceEv(VirtualClock& clk, uint64_t msg_id, uint8_t stage, uint64_t value) {
  Event e{};
  e.ts_mono_ns = clk.MonoNanos();
  e.entity_id = msg_id;
  e.stage = stage;
  e.value = value;
  e.kind = static_cast<uint8_t>(EventKind::kTrace);
  return e;
}

Event EventEv(VirtualClock& clk, uint64_t key, uint8_t level) {
  Event e{};
  e.ts_mono_ns = clk.MonoNanos();
  e.key = key;
  e.stage = level;
  e.kind = static_cast<uint8_t>(EventKind::kEvent);
  return e;
}

// 拆批：走公共 BatchView（明文/压缩两种线上格式都合法）
bool ParseBatchRecords(const std::string& payload, std::vector<std::string>* records) {
  imobs::BatchView v;
  if (!imobs::ParseBatchView(payload.data(), payload.size(), &v)) return false;
  records->clear();
  for (const auto& r : v.records) records->emplace_back(r);
  return true;
}

}  // namespace

TEST(Kernel, MetricsEndToEndThroughChannel) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  const uint64_t kc = reg.RegisterCounter("k.c", {LabelDef{"net", {"wifi", "cell"}}});
  const uint64_t kh = reg.RegisterHistogram("k.h", {});
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(), &err);
  ASSERT_TRUE(core != nullptr);

  for (int i = 0; i < 3; ++i) ASSERT_TRUE(core->Emit(MetricEv(clk, kc, 1, 2)));
  for (uint64_t v = 1; v <= 100; ++v) ASSERT_TRUE(core->Emit(MetricEv(clk, kh, 0, v)));
  core->Step();
  EXPECT_EQ(ch.sent.size(), size_t{0});  // 快照窗口未到，什么都不该发

  clk.AdvanceMillis(30'000);
  core->Step();  // 快照 → P2 成批 → 发送，一步完成
  ASSERT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP2);

  std::vector<std::string> records;
  ASSERT_TRUE(ParseBatchRecords(ch.sent[0].payload, &records));
  ASSERT_EQ(records.size(), size_t{1});
  std::vector<imobs::MetricsRowView> rows;
  ASSERT_TRUE(DecodeMetricsChunk(
      reinterpret_cast<const uint8_t*>(records[0].data()), records[0].size(), &rows));
  ASSERT_EQ(rows.size(), size_t{2});
  bool checked_counter = false;
  bool checked_hist = false;
  for (auto& row : rows) {
    if (row.key == kc) {
      EXPECT_EQ(row.kind, 0);
      EXPECT_EQ(row.label_idx, 1);
      EXPECT_EQ(row.count, 3u);
      EXPECT_EQ(row.sum, 6u);
      checked_counter = true;
    } else if (row.key == kh) {
      EXPECT_EQ(row.kind, 1);
      EXPECT_EQ(row.count, 100u);
      const double p50 = row.digest.Quantile(0.50);
      EXPECT_TRUE(p50 >= 49.0 && p50 <= 52.0);
      checked_hist = true;
    }
  }
  EXPECT_TRUE(checked_counter);
  EXPECT_TRUE(checked_hist);
  EXPECT_EQ(core->self_stats().drained, 103u);
}

TEST(Kernel, TraceChunkByCountThenByAge) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(), &err);
  ASSERT_TRUE(core != nullptr);

  uint64_t first_ts = 0;
  for (uint8_t i = 0; i < 4; ++i) {  // max_events=4：按量冲刷
    clk.AdvanceMillis(1);
    if (i == 0) first_ts = clk.MonoNanos();
    ASSERT_TRUE(core->Emit(TraceEv(clk, 0x9000 + i, i, 100u + i)));
  }
  core->Step();
  ASSERT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP1);

  std::vector<std::string> records;
  ASSERT_TRUE(ParseBatchRecords(ch.sent[0].payload, &records));
  ASSERT_EQ(records.size(), size_t{1});
  uint64_t base = 0;
  std::vector<imobs::TraceEventWire> evs;
  ASSERT_TRUE(DecodeTraceChunk(reinterpret_cast<const uint8_t*>(records[0].data()),
                               records[0].size(), &base, &evs));
  EXPECT_EQ(base, first_ts);  // base = 块内最小 ts
  ASSERT_EQ(evs.size(), size_t{4});
  EXPECT_EQ(evs[0].msg_id, 0x9000u);
  EXPECT_EQ(evs[0].ts_delta_ns, 0u);
  EXPECT_EQ(evs[3].msg_id, 0x9003u);
  EXPECT_EQ(evs[3].ts_delta_ns, 3'000'000u);  // 每事件间隔 1ms

  // 不足量的两条：按龄（5s）冲刷
  clk.AdvanceMillis(1);
  ASSERT_TRUE(core->Emit(TraceEv(clk, 0xA001, 7, 1)));
  ASSERT_TRUE(core->Emit(TraceEv(clk, 0xA002, 8, 2)));
  core->Step();
  EXPECT_EQ(ch.sent.size(), size_t{1});  // 未到量、未到龄
  clk.AdvanceMillis(5'000);
  core->Step();
  ASSERT_EQ(ch.sent.size(), size_t{2});
  ASSERT_TRUE(ParseBatchRecords(ch.sent[1].payload, &records));
  ASSERT_TRUE(DecodeTraceChunk(reinterpret_cast<const uint8_t*>(records[0].data()),
                               records[0].size(), &base, &evs));
  EXPECT_EQ(evs.size(), size_t{2});
}

TEST(Kernel, EventLevelsRouteToPriorities) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(), &err);
  ASSERT_TRUE(core != nullptr);

  ASSERT_TRUE(core->Emit(EventEv(clk, 0xE0, 0)));  // P0：即时
  core->Step();
  ASSERT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP0);

  clk.AdvanceMillis(2);
  ASSERT_TRUE(core->Emit(EventEv(clk, 0xE2, 2)));  // P2：随周期
  core->Step();
  ASSERT_EQ(ch.sent.size(), size_t{2});
  EXPECT_TRUE(ch.sent[1].pri == Priority::kP2);

  std::vector<std::string> records;
  ASSERT_TRUE(ParseBatchRecords(ch.sent[0].payload, &records));
  imobs::EventRecordWire rec;
  ASSERT_TRUE(DecodeEventRecord(reinterpret_cast<const uint8_t*>(records[0].data()),
                                records[0].size(), &rec));
  EXPECT_EQ(rec.key, 0xE0u);
  EXPECT_EQ(rec.level, 0);
}

TEST(Kernel, ShutdownFlushesEverything) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  const uint64_t kc = reg.RegisterCounter("s.c", {});
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, KOpt(), &err);
  ASSERT_TRUE(core != nullptr);

  clk.AdvanceMillis(3);
  ASSERT_TRUE(core->Emit(MetricEv(clk, kc, 0, 1)));
  ASSERT_TRUE(core->Emit(TraceEv(clk, 0xBEEF, 4, 9)));
  ASSERT_TRUE(core->Emit(EventEv(clk, 0xE1, 1)));
  core->Shutdown();  // 不经任何 Step：终局冲刷必须自己收干净

  ASSERT_EQ(ch.sent.size(), size_t{2});  // P1（事件 + 轨迹块同批）、P2（强制快照）
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP1);
  EXPECT_TRUE(ch.sent[1].pri == Priority::kP2);
  std::vector<std::string> records;
  ASSERT_TRUE(ParseBatchRecords(ch.sent[0].payload, &records));
  ASSERT_EQ(records.size(), size_t{2});
  EXPECT_EQ(records[0][0], 'E');  // 提交序：drain 分发的事件在前
  EXPECT_EQ(records[1][0], 'T');  // 轨迹块冲刷在后
  EXPECT_EQ(core->pipeline().PendingBatches(Priority::kP1), 0u);
  EXPECT_EQ(core->pipeline().PendingBatches(Priority::kP2), 0u);
}

TEST(Kernel, RingDropIsAccounted) {
  VirtualClock clk;
  FakeChannel ch;
  SchemaRegistry reg;
  const uint64_t kc = reg.RegisterCounter("d.c", {});
  auto opt = KOpt();
  opt.ring_shards = 1;
  opt.ring_capacity_per_shard = 4;
  std::string err;
  auto core = KernelCore::Open(TmpDir(), &clk, &ch, &reg, opt, &err);
  ASSERT_TRUE(core != nullptr);

  int ok = 0;
  for (int i = 0; i < 10; ++i) {
    if (core->Emit(MetricEv(clk, kc, 0, 1))) ++ok;
  }
  EXPECT_EQ(ok, 4);
  core->Step();
  const auto s = core->self_stats();
  EXPECT_EQ(s.drained, 4u);
  EXPECT_EQ(s.ring_dropped, 6u);  // 丢了多少自己要有账（压舱石第二条）
}

TEST(Kernel, ThreadShellSmokeRealClock) {
  // 真实线程 + 真实时钟：热路径 API 跨线程 → 消费线程 → 通道全链冒烟（TSan 目标）
  FakeChannel ch;
  SchemaRegistry reg;
  const uint64_t kc = reg.RegisterCounter("t.c", {});
  auto opt = KOpt();
  opt.metrics_snapshot_interval_ms = 50;
  opt.trace_chunk_max_age_ms = 20;
  opt.poll_interval_ms = 1;
  std::string err;
  auto k = ObsKernel::Start(TmpDir(), &ch, &reg, opt, &err);
  ASSERT_TRUE(k != nullptr);

  k->AddCounter(kc, 0, 1);
  k->TraceStage(0x5A5A, 4, 0, 7, 42);
  k->EmitEvent(0xC0DE, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  k->Stop();

  bool saw_p0 = false;
  bool saw_p1 = false;
  bool saw_p2 = false;
  for (const auto& s : ch.sent) {
    if (s.pri == Priority::kP0) saw_p0 = true;
    if (s.pri == Priority::kP1) saw_p1 = true;
    if (s.pri == Priority::kP2) saw_p2 = true;
  }
  EXPECT_TRUE(saw_p0);  // P0 事件即时
  EXPECT_TRUE(saw_p1);  // 轨迹块按龄（20ms ≪ 250ms）
  EXPECT_TRUE(saw_p2);  // 指标快照（50ms ≪ 250ms）
  EXPECT_EQ(k->core().self_stats().ring_dropped, 0u);
}

IMOBS_TEST_MAIN
