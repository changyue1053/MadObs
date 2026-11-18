// 拼接器单测：输入不是构造的理想数据，而是真实 UploadPipeline 产出的批字节。
#include "imobs/stitcher.h"

#include <unistd.h>

#include <random>
#include <string>
#include <vector>

#include "imobs/batch_view.h"
#include "imobs/clock.h"
#include "imobs/pipeline.h"
#include "imobs/records.h"
#include "imobs_test.h"

using imobs::IChannel;
using imobs::PipelineOptions;
using imobs::Priority;
using imobs::TraceEventWire;
using imobs::TraceStitcher;
using imobs::UploadPipeline;
using imobs::VirtualClock;

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_stitch_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

struct FakeChannel final : IChannel {
  std::vector<std::string> payloads;
  void Send(Priority, uint64_t, std::string_view payload, SendCallback cb) override {
    payloads.emplace_back(payload);
    cb({true});
  }
};

std::vector<uint8_t> Chunk(uint64_t base, const std::vector<TraceEventWire>& evs) {
  std::vector<uint8_t> buf;
  EncodeTraceChunk(base, evs, &buf);
  return buf;
}

TraceEventWire Ev(uint64_t msg_id, uint8_t stage) {
  TraceEventWire e;
  e.msg_id = msg_id;
  e.stage = stage;
  return e;
}

}  // namespace

TEST(Stitcher, IngestsRealPipelineBatches) {
  VirtualClock clk;
  FakeChannel ch;
  PipelineOptions opt;
  opt.p1_flush_bytes = 1;
  for (auto& w : opt.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  std::string err;
  auto p = UploadPipeline::Open(TmpDir(), &clk, &ch, opt, &err);
  ASSERT_TRUE(p != nullptr);

  // 批 1：轨迹块（A:T0,T1 + B:T4）与一条非轨迹记录（'E'）混批——拼接器只吃 'T'
  const auto chunk1 = Chunk(100, {Ev(0xA, 0), Ev(0xA, 1), Ev(0xB, 4)});
  ASSERT_TRUE(p->Submit(Priority::kP1, std::string_view(
      reinterpret_cast<const char*>(chunk1.data()), chunk1.size())));
  imobs::EventRecordWire evrec{1, 0xE1, 5, 0, 0};
  std::vector<uint8_t> evbuf;
  EncodeEventRecord(evrec, &evbuf);
  ASSERT_TRUE(p->Submit(Priority::kP1, std::string_view(
      reinterpret_cast<const char*>(evbuf.data()), evbuf.size())));
  p->Poll();
  ASSERT_EQ(ch.payloads.size(), size_t{1});

  // 批 2：A 的补充节点（晚到块的拼接语义）
  const auto chunk2 = Chunk(200, {Ev(0xA, 2)});
  ASSERT_TRUE(p->Submit(Priority::kP1, std::string_view(
      reinterpret_cast<const char*>(chunk2.data()), chunk2.size())));
  p->Poll();
  ASSERT_EQ(ch.payloads.size(), size_t{2});

  TraceStitcher st;
  EXPECT_EQ(st.IngestBatch(ch.payloads[0].data(), ch.payloads[0].size()), size_t{3});
  EXPECT_EQ(st.IngestBatch(ch.payloads[1].data(), ch.payloads[1].size()), size_t{1});
  EXPECT_EQ(st.JourneyCount(), size_t{2});
  EXPECT_EQ(st.trace_events(), 4u);

  const auto stages_a = st.StagesOf(0xA);
  ASSERT_EQ(stages_a.size(), size_t{3});
  EXPECT_EQ(stages_a[0], 0);
  EXPECT_EQ(stages_a[2], 2);
  EXPECT_TRUE(st.Complete(0xA, {0, 1, 2}));
  EXPECT_FALSE(st.Complete(0xA, {0, 1, 2, 3}));  // 断链于 T3：完整性判定的反面
  EXPECT_FALSE(st.Complete(0xC, {0}));           // 不存在的消息
  EXPECT_TRUE(st.StagesOf(0xC).empty());

  // 批视图解析同源校验
  imobs::BatchView view;
  ASSERT_TRUE(imobs::ParseBatchView(ch.payloads[0].data(), ch.payloads[0].size(), &view));
  EXPECT_EQ(view.pri, 1);
  EXPECT_EQ(view.records.size(), size_t{2});
}

TEST(Stitcher, GarbageIsRejectedNotCrashed) {
  TraceStitcher st;
  std::minstd_rand rng(31);
  std::vector<uint8_t> buf;
  for (int i = 0; i < 10'000; ++i) {
    buf.resize(rng() % 128);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    st.IngestBatch(buf.data(), buf.size());
  }
  EXPECT_EQ(st.JourneyCount(), size_t{0});  // 随机字节永远进不了账
}

IMOBS_TEST_MAIN
