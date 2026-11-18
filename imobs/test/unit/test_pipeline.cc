// 上报管线确定性剧本（实施计划 P2.4/P2.5 DoD）：
// 全部时间行为由 VirtualClock 驱动，退避种子固定——每个用例毫秒级、100% 可复现。
#include "imobs/pipeline.h"

#include <unistd.h>

#include <string>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs/clock.h"
#include "imobs/wire.h"
#include "imobs_test.h"

using imobs::IChannel;
using imobs::PipelineOptions;
using imobs::Priority;
using imobs::UploadPipeline;
using imobs::VirtualClock;
using imobs::WireReader;

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_pipe_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

struct FakeChannel final : IChannel {
  struct SentBatch {
    Priority pri;
    uint64_t id;
    std::string payload;
  };
  std::vector<SentBatch> sent;
  int fail_next = 0;  // 前 N 次 Send 回失败（同步回调）

  void Send(Priority pri, uint64_t id, std::string_view payload, SendCallback cb) override {
    sent.push_back({pri, id, std::string(payload)});
    bool ok = true;
    if (fail_next > 0) {
      --fail_next;
      ok = false;
    }
    cb({ok});
  }
};

PipelineOptions TinyOpt() {
  PipelineOptions o;
  o.p1_flush_interval_ms = 30'000;
  o.p1_flush_bytes = 64;
  o.p2_flush_interval_ms = 300'000;
  o.day_quota_bytes = 1'000'000;
  o.retry_base_ms = 1'000;
  o.retry_cap_ms = 60'000;
  o.rng_seed = 7;
  for (auto& w : o.wal) {
    w.segment_size = 4096;
    w.segment_count = 2;
  }
  return o;
}

// 批头解析：跳过 batch_id 后校验 magic，取出 pri 与记录数
bool ParseBatch(const std::string& payload, uint8_t* pri, uint64_t* records) {
  WireReader r(payload.data(), payload.size());
  uint64_t id = 0;
  const uint8_t* magic = nullptr;
  uint64_t wall = 0;
  uint64_t mono = 0;
  if (!r.Fixed64(&id) || !r.Bytes(&magic, 4)) return false;
  if (magic[0] != 'I' || magic[1] != 'M' || magic[2] != 'B' || magic[3] != '1') return false;
  return r.U8(pri) && r.Fixed64(&wall) && r.Fixed64(&mono) && r.Varint(records);
}

}  // namespace

TEST(Pipeline, P0ImmediateSendAndAck) {
  VirtualClock clk;
  FakeChannel ch;
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, TinyOpt(), &err);
  ASSERT_TRUE(p != nullptr);

  ASSERT_TRUE(p->Submit(Priority::kP0, "p0-crash-event"));
  EXPECT_EQ(p->PendingBatches(Priority::kP0), 1u);  // Submit 即成批并持久化
  p->Poll();
  ASSERT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(ch.sent[0].pri == Priority::kP0);
  uint8_t pri = 0;
  uint64_t records = 0;
  ASSERT_TRUE(ParseBatch(ch.sent[0].payload, &pri, &records));
  EXPECT_EQ(records, 1u);
  EXPECT_EQ(p->PendingBatches(Priority::kP0), 0u);  // ACK 推进水位
  EXPECT_EQ(p->stats().acks, 1u);
}

TEST(Pipeline, P1FlushByBytesThenByTime) {
  VirtualClock clk;
  FakeChannel ch;
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, TinyOpt(), &err);
  ASSERT_TRUE(p != nullptr);

  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(p->Submit(Priority::kP1, "record-0x"));  // 每帧 ~10B，8 帧 > 64B 阈值
  }
  p->Poll();  // 按字节成批 + 发送
  ASSERT_EQ(ch.sent.size(), size_t{1});
  uint8_t pri = 0;
  uint64_t records = 0;
  ASSERT_TRUE(ParseBatch(ch.sent[0].payload, &pri, &records));
  EXPECT_EQ(records, 8u);

  ASSERT_TRUE(p->Submit(Priority::kP1, "tail-1"));
  ASSERT_TRUE(p->Submit(Priority::kP1, "tail-2"));
  p->Poll();  // 未达字节阈值、未到时间 → 不成批
  EXPECT_EQ(ch.sent.size(), size_t{1});
  clk.AdvanceMillis(30'000);
  p->Poll();  // 到 30s 窗口 → 按时间成批
  ASSERT_EQ(ch.sent.size(), size_t{2});
  ASSERT_TRUE(ParseBatch(ch.sent[1].payload, &pri, &records));
  EXPECT_EQ(records, 2u);
  EXPECT_EQ(p->stats().batches_built, 2u);
}

TEST(Pipeline, CompressedBatchRoundTripAndP0StaysPlain) {
  VirtualClock clk;
  FakeChannel ch;
  std::string err;
  PipelineOptions opt = TinyOpt();
  opt.compress_min_bytes = 64;  // 测试确定性：批体一定跨过压缩阈值
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, opt, &err);
  ASSERT_TRUE(p != nullptr);

  // 高重复记录（真实批的形态）：40 × 33B ≈ 1.3KB，远超 64B flush 与压缩阈值。
  const std::string rec = "trace|msg=42|stage=T4|status=ok;";
  size_t raw_sum = 0;
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(p->Submit(Priority::kP1, rec));
    raw_sum += rec.size();
  }
  p->Poll();
  ASSERT_EQ(ch.sent.size(), size_t{1});
  EXPECT_LT(ch.sent[0].payload.size(), raw_sum / 2);  // 网络字节至少减半

  imobs::BatchView v;
  ASSERT_TRUE(imobs::ParseBatchView(ch.sent[0].payload.data(), ch.sent[0].payload.size(), &v));
  EXPECT_TRUE(v.compressed);
  ASSERT_EQ(v.records.size(), size_t{40});
  for (const auto& r : v.records) EXPECT_TRUE(r == rec);

  // P0 永远明文：即使批体够大也不压（崩溃路径求简 + 单条即发）。
  const std::string big(600, 'x');
  ASSERT_TRUE(p->Submit(Priority::kP0, big));
  p->Poll();
  ASSERT_EQ(ch.sent.size(), size_t{2});
  imobs::BatchView v0;
  ASSERT_TRUE(imobs::ParseBatchView(ch.sent[1].payload.data(), ch.sent[1].payload.size(), &v0));
  EXPECT_TRUE(!v0.compressed);
  ASSERT_EQ(v0.records.size(), size_t{1});
  EXPECT_TRUE(v0.records[0] == big);
}

TEST(Pipeline, RetryBackoffBounded) {
  VirtualClock clk;
  FakeChannel ch;
  ch.fail_next = 2;
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, TinyOpt(), &err);
  ASSERT_TRUE(p != nullptr);

  ASSERT_TRUE(p->Submit(Priority::kP0, "retry-me"));
  p->Poll();  // 尝试 #1：失败
  EXPECT_EQ(ch.sent.size(), size_t{1});
  p->Poll();  // 仍在退避窗口内（jitter ≥ 1ms，时间未动）→ 不重发
  EXPECT_EQ(ch.sent.size(), size_t{1});

  clk.AdvanceMillis(1'000);  // 尝试 1 的窗口 = base，推进 base 必然越过 full-jitter
  p->Poll();                 // 尝试 #2：失败
  EXPECT_EQ(ch.sent.size(), size_t{2});
  clk.AdvanceMillis(2'000);  // 尝试 2 的窗口 = 2×base
  p->Poll();                 // 尝试 #3：成功
  ASSERT_EQ(ch.sent.size(), size_t{3});
  EXPECT_EQ(p->stats().fails, 2u);
  EXPECT_EQ(p->stats().acks, 1u);
  EXPECT_EQ(p->PendingBatches(Priority::kP0), 0u);
  // 三次尝试的 batch_id 必须一致——重试重放的是同一段字节
  EXPECT_EQ(ch.sent[0].id, ch.sent[1].id);
  EXPECT_EQ(ch.sent[1].id, ch.sent[2].id);
}

TEST(Pipeline, QuotaHoldsP1NotP0) {
  VirtualClock clk;
  FakeChannel ch;
  auto opt = TinyOpt();
  // 配额检查发生在发送前（used >= quota 即扣住）：quota=1 表达"首批放行、
  // 发出即计费、随后必耗尽"，不依赖具体批体积。
  opt.day_quota_bytes = 1;
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, opt, &err);
  ASSERT_TRUE(p != nullptr);

  ASSERT_TRUE(p->Submit(Priority::kP1, "first-batch-record"));
  p->FlushAll();
  p->Poll();  // 发出（发出即计费 → 耗尽）
  EXPECT_EQ(ch.sent.size(), size_t{1});

  ASSERT_TRUE(p->Submit(Priority::kP1, "second-batch-record"));
  p->FlushAll();
  p->Poll();  // P1 被预算扣住
  EXPECT_EQ(ch.sent.size(), size_t{1});
  EXPECT_TRUE(p->stats().quota_deferred >= 1);
  EXPECT_EQ(p->PendingBatches(Priority::kP1), 1u);  // 押在 WAL 里不丢

  ASSERT_TRUE(p->Submit(Priority::kP0, "p0-must-pass"));
  p->Poll();  // P0 不受预算约束——它就是为故障时刻设计的
  EXPECT_EQ(ch.sent.size(), size_t{2});
  EXPECT_TRUE(ch.sent[1].pri == Priority::kP0);

  clk.AdvanceMillis(86'400'000);  // 预算按自然日重置
  p->Poll();
  EXPECT_EQ(ch.sent.size(), size_t{3});
  EXPECT_EQ(p->PendingBatches(Priority::kP1), 0u);
}

TEST(Pipeline, BackgroundHoldsP1ForegroundResumes) {
  VirtualClock clk;
  FakeChannel ch;
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, TinyOpt(), &err);
  ASSERT_TRUE(p != nullptr);

  p->SetForeground(false);
  ASSERT_TRUE(p->Submit(Priority::kP1, "bg-held"));
  p->FlushAll();
  p->Poll();
  EXPECT_EQ(ch.sent.size(), size_t{0});  // 后台：P1 停发

  ASSERT_TRUE(p->Submit(Priority::kP0, "bg-p0-pass"));
  p->Poll();
  EXPECT_EQ(ch.sent.size(), size_t{1});  // 后台：P0 照发

  p->SetForeground(true);
  p->Poll();
  EXPECT_EQ(ch.sent.size(), size_t{2});  // 回前台：P1 补发
}

TEST(Pipeline, CrashRedeliveryKeepsBatchId) {
  const std::string dir = TmpDir();
  uint64_t id0 = 0;
  std::string payload0;
  {
    VirtualClock clk;
    FakeChannel down;
    down.fail_next = 1'000'000;  // 通道全程不可用
    std::string err;
    auto a = UploadPipeline::Open(dir, &clk, &down, TinyOpt(), &err);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(a->Submit(Priority::kP1, "survive-restart"));
    a->FlushAll();
    a->Poll();
    ASSERT_EQ(down.sent.size(), size_t{1});
    id0 = down.sent[0].id;
    payload0 = down.sent[0].payload;
  }  // 对象销毁 ≈ 进程退出；真正 kill -9 的等价性由 test_wal_recovery 覆盖

  VirtualClock clk;
  FakeChannel up;
  std::string err;
  auto b = UploadPipeline::Open(dir, &clk, &up, TinyOpt(), &err);
  ASSERT_TRUE(b != nullptr);
  EXPECT_EQ(b->PendingBatches(Priority::kP1), 1u);  // WAL 里活着
  b->Poll();
  ASSERT_EQ(up.sent.size(), size_t{1});
  EXPECT_EQ(up.sent[0].id, id0);            // 内容派生 id：重发天然同 id
  EXPECT_TRUE(up.sent[0].payload == payload0);  // 字节级一致 → 服务端按 id 去重即幂等
  EXPECT_EQ(b->PendingBatches(Priority::kP1), 0u);
}

TEST(Pipeline, RestartAfterAckNoDuplicate) {
  const std::string dir = TmpDir();
  {
    VirtualClock clk;
    FakeChannel ch;
    std::string err;
    auto a = UploadPipeline::Open(dir, &clk, &ch, TinyOpt(), &err);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(a->Submit(Priority::kP1, "acked-once"));
    a->FlushAll();
    a->Poll();
    ASSERT_EQ(ch.sent.size(), size_t{1});  // 已 ACK，水位已持久化
  }
  VirtualClock clk;
  FakeChannel ch;
  std::string err;
  auto b = UploadPipeline::Open(dir, &clk, &ch, TinyOpt(), &err);
  ASSERT_TRUE(b != nullptr);
  EXPECT_EQ(b->PendingBatches(Priority::kP1), 0u);
  b->Poll();
  clk.AdvanceMillis(60'000);
  b->Poll();
  EXPECT_EQ(ch.sent.size(), size_t{0});  // 已消费的批绝不复活
}

IMOBS_TEST_MAIN
