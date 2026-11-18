// 回环端到端（M3 前奏，实施计划 P8 的 hermetic 形态）：
// 三个内核实例（发送端设备 / 服务端 / 接收端设备）各自经真实 TCP socket 上报，
// 收集器做 batch_id 幂等 + 轨迹拼接——断言 20 条消息的 T0~T9 物流单全部拼齐；
// 随后用原始字节重放一个批，验证服务端幂等（重复批 ACK 但不入账）。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imobs/collector.h"
#include "imobs/obs_kernel.h"
#include "imobs/tcp_channel.h"
#include "imobs_test.h"

using imobs::Collector;
using imobs::KernelOptions;
using imobs::ObsKernel;
using imobs::SchemaRegistry;
using imobs::TcpChannel;

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_e2e_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

// 声明顺序即析构契约：kernel（后声明）先析构，channel 后析构——
// 关停期通道线程的迟到回调经完成队列安全落入孤儿收件箱。
struct Node {
  std::unique_ptr<TcpChannel> channel;
  SchemaRegistry schema;
  std::unique_ptr<ObsKernel> kernel;
};

std::unique_ptr<Node> MakeNode(uint16_t collector_port, std::string* err) {
  auto n = std::make_unique<Node>();
  TcpChannel::Options copt;
  copt.port = collector_port;
  n->channel = std::make_unique<TcpChannel>(copt);
  KernelOptions o;
  o.trace_chunk_max_age_ms = 20;
  o.poll_interval_ms = 1;
  o.pipeline.p1_flush_bytes = 1;
  for (auto& w : o.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  n->kernel = ObsKernel::Start(TmpDir(), n->channel.get(), &n->schema, o, err);
  return n;
}

bool RawReplay(uint16_t port, const std::vector<uint8_t>& batch, uint64_t* acked_id) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  const auto len = static_cast<uint32_t>(batch.size());
  bool ok = ::send(fd, &len, 4, 0) == 4 &&
            ::send(fd, batch.data(), batch.size(), 0) == static_cast<ssize_t>(batch.size()) &&
            ::recv(fd, acked_id, 8, MSG_WAITALL) == 8;
  ::close(fd);
  return ok;
}

}  // namespace

TEST(E2eLoopback, ThreeKernelsStitchFullJourneysThenReplayIsDeduped) {
  Collector col;
  std::string err;
  ASSERT_TRUE(col.Start(0, &err));

  auto sender = MakeNode(col.port(), &err);
  ASSERT_TRUE(sender->kernel != nullptr);
  auto server = MakeNode(col.port(), &err);
  ASSERT_TRUE(server->kernel != nullptr);
  auto receiver = MakeNode(col.port(), &err);
  ASSERT_TRUE(receiver->kernel != nullptr);

  constexpr int kMsgs = 20;
  constexpr uint64_t kBase = 0xE2E00000;
  for (int i = 0; i < kMsgs; ++i) {
    const uint64_t id = kBase + static_cast<uint64_t>(i);
    for (uint8_t s = 0; s <= 2; ++s) sender->kernel->TraceStage(id, s, 0);    // T0~T2
    for (uint8_t s = 3; s <= 6; ++s) server->kernel->TraceStage(id, s, 0);    // T3~T6
    for (uint8_t s = 7; s <= 9; ++s) receiver->kernel->TraceStage(id, s, 0);  // T7~T9
  }

  // 真实网络：轮询等待全链路收敛（上限 5s，常态 <200ms）
  const std::vector<uint8_t> kAllStages = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  bool converged = false;
  for (int t = 0; t < 500 && !converged; ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (col.JourneyCount() != static_cast<size_t>(kMsgs)) continue;
    converged = true;
    for (int i = 0; i < kMsgs && converged; ++i) {
      converged = col.JourneyComplete(kBase + static_cast<uint64_t>(i), kAllStages);
    }
  }
  ASSERT_TRUE(converged);  // 20 张物流单，T0~T9 一站不缺
  EXPECT_EQ(col.stats().trace_events, 200u);
  EXPECT_EQ(col.stats().duplicates, 0u);

  sender->kernel->Stop();
  server->kernel->Stop();
  receiver->kernel->Stop();

  // —— 重放注入：同一批原始字节直接经 raw socket 再投 ——
  const auto sample = col.SampleBatch();
  ASSERT_TRUE(!sample.empty());
  uint64_t sample_id = 0;
  std::memcpy(&sample_id, sample.data(), 8);
  const auto before = col.stats();

  uint64_t acked = 0;
  ASSERT_TRUE(RawReplay(col.port(), sample, &acked));
  EXPECT_EQ(acked, sample_id);  // 重复批照样 ACK（端侧只需要"已收到"这个事实）

  const auto after = col.stats();
  EXPECT_EQ(after.duplicates, before.duplicates + 1);      // 幂等命中有账
  EXPECT_EQ(after.batches, before.batches);                // 不重复入账
  EXPECT_EQ(after.trace_events, before.trace_events);      // 旅程账目纹丝不动
  EXPECT_EQ(col.JourneyCount(), static_cast<size_t>(kMsgs));

  col.Stop();
}

IMOBS_TEST_MAIN
