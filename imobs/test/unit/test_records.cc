#include "imobs/records.h"

#include <random>
#include <vector>

#include "imobs_test.h"

using imobs::DecodeEventRecord;
using imobs::DecodeMetricsChunk;
using imobs::DecodeTraceChunk;
using imobs::EncodeEventRecord;
using imobs::EncodeMetricsChunk;
using imobs::EncodeTraceChunk;
using imobs::EventRecordWire;
using imobs::MetricsRowView;
using imobs::MetricsTable;
using imobs::SchemaRegistry;
using imobs::TraceEventWire;

TEST(Records, MetricsChunkRoundtrip) {
  std::vector<MetricsTable::Row> rows;
  MetricsTable::Row counter;
  counter.key = 0x1111;
  counter.label_idx = 3;
  counter.kind = SchemaRegistry::MetricKind::kCounter;
  counter.count = 42;
  counter.sum = 420;
  rows.push_back(std::move(counter));

  MetricsTable::Row hist;
  hist.key = 0x2222;
  hist.kind = SchemaRegistry::MetricKind::kHistogram;
  hist.count = 1000;
  hist.sum = 500500;
  for (int v = 1; v <= 1000; ++v) hist.digest.Add(v);
  rows.push_back(std::move(hist));

  std::vector<uint8_t> buf;
  EncodeMetricsChunk(rows, &buf);

  std::vector<MetricsRowView> back;
  ASSERT_TRUE(DecodeMetricsChunk(buf.data(), buf.size(), &back));
  ASSERT_EQ(back.size(), size_t{2});
  EXPECT_EQ(back[0].kind, 0);
  EXPECT_EQ(back[0].key, 0x1111u);
  EXPECT_EQ(back[0].label_idx, 3);
  EXPECT_EQ(back[0].count, 42u);
  EXPECT_EQ(back[0].sum, 420u);
  EXPECT_EQ(back[1].kind, 1);
  EXPECT_EQ(back[1].digest.Count(), 1000u);
  const double p50 = back[1].digest.Quantile(0.50);
  EXPECT_TRUE(p50 >= 495.0 && p50 <= 506.0);  // 序列化往返后精度不损

  // 截断必须被拒绝，绝不吐半解析结果
  std::vector<MetricsRowView> junk;
  EXPECT_FALSE(DecodeMetricsChunk(buf.data(), buf.size() - 1, &junk));
}

TEST(Records, TraceChunkRoundtrip) {
  std::vector<TraceEventWire> events;
  for (int i = 0; i < 5; ++i) {
    TraceEventWire ev;
    ev.msg_id = 0xABCD0000 + static_cast<uint64_t>(i);
    ev.stage = static_cast<uint8_t>(i);
    ev.status = 0;
    ev.ts_delta_ns = static_cast<uint64_t>(i) * 1'000'000;
    ev.n_attrs = 1;
    ev.attrs[0] = {imobs::kAttrValue, 10'000u + static_cast<uint64_t>(i)};
    events.push_back(ev);
  }
  std::vector<uint8_t> buf;
  EncodeTraceChunk(777'000'000, events, &buf);

  uint64_t base = 0;
  std::vector<TraceEventWire> back;
  ASSERT_TRUE(DecodeTraceChunk(buf.data(), buf.size(), &base, &back));
  EXPECT_EQ(base, 777'000'000u);
  ASSERT_EQ(back.size(), size_t{5});
  EXPECT_EQ(back[4].msg_id, 0xABCD0004u);
  EXPECT_EQ(back[4].ts_delta_ns, 4'000'000u);
  EXPECT_EQ(back[4].attrs[0].value, 10'004u);
}

TEST(Records, EventRecordRoundtrip) {
  EventRecordWire rec{0, 0xFEED, 123'456'789, 7, 9};
  std::vector<uint8_t> buf;
  EncodeEventRecord(rec, &buf);
  EventRecordWire back;
  ASSERT_TRUE(DecodeEventRecord(buf.data(), buf.size(), &back));
  EXPECT_EQ(back.level, 0);
  EXPECT_EQ(back.key, 0xFEEDu);
  EXPECT_EQ(back.ts_mono_ns, 123'456'789u);
  EXPECT_EQ(back.a, 7u);
  EXPECT_EQ(back.b, 9u);
}

TEST(Records, HealRecordRoundtrip) {
  imobs::HealRecordWire rec;
  rec.scene_id = 0xC0FFEE;
  rec.from = 3;   // Healing
  rec.to = 5;     // Exhausted
  rec.ladder_index = 2;
  rec.reason = 13;  // kExhaustedLadder
  rec.ts_ms = 987'654;
  rec.action = "rebuild_db";
  std::vector<uint8_t> buf;
  EncodeHealRecord(rec, &buf);
  imobs::HealRecordWire back;
  ASSERT_TRUE(DecodeHealRecord(buf.data(), buf.size(), &back));
  EXPECT_EQ(back.scene_id, 0xC0FFEEu);
  EXPECT_EQ(back.from, 3);
  EXPECT_EQ(back.to, 5);
  EXPECT_EQ(back.ladder_index, 2);
  EXPECT_EQ(back.reason, 13);
  EXPECT_TRUE(back.action == "rebuild_db");
  EXPECT_FALSE(DecodeHealRecord(buf.data(), buf.size() - 1, &back));  // 截断拒绝
}

TEST(Records, DecodersSurviveRandomBytes) {
  // 四个解码器都会在服务端处理对端数据：任意输入只允许返回 false（ASan 兜底越界）
  std::minstd_rand rng(23);
  std::vector<uint8_t> buf;
  for (int iter = 0; iter < 30'000; ++iter) {
    const size_t n = rng() % 96;
    buf.resize(n);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    std::vector<MetricsRowView> rows;
    (void)DecodeMetricsChunk(buf.data(), buf.size(), &rows);
    uint64_t base = 0;
    std::vector<TraceEventWire> evs;
    (void)DecodeTraceChunk(buf.data(), buf.size(), &base, &evs);
    EventRecordWire rec;
    (void)DecodeEventRecord(buf.data(), buf.size(), &rec);
    imobs::HealRecordWire heal;
    (void)DecodeHealRecord(buf.data(), buf.size(), &heal);
  }
  EXPECT_TRUE(true);
}

IMOBS_TEST_MAIN
