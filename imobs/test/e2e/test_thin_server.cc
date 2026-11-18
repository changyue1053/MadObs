// 薄服务端 e2e（P7 单机形态 DoD）：
// 真实内核（线程壳 + TcpChannel）→ ingest（幂等 + 拼接 + 聚合）→ HTTP 查询面。
// 断言四类信号的端到端语义：指标合并（含 t-digest 分位）、消息旅程、
// 回捞日志按命令对账、健康页统计。
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

#include "imobs/clock.h"
#include "imobs/obs_kernel.h"
#include "imobs/policy.h"
#include "imobs/tcp_channel.h"
#include "imobs/thin_server.h"
#include "imobs_test.h"

using imobs::Command;
using imobs::CommandType;
using imobs::EncodeCommand;
using imobs::kEnvelopeCommand;
using imobs::KernelOptions;
using imobs::ObsKernel;
using imobs::SchemaRegistry;
using imobs::SignEnvelope;
using imobs::TcpChannel;
using imobs::ThinServer;

namespace {

std::string TmpDir() {
  char tmpl[] = "/tmp/imobs_thin_XXXXXX";
  return std::string(mkdtemp(tmpl));
}

// 极简 HTTP GET（测试专用）：连接 → 请求 → 读到对端关闭 → 取 body。
std::string HttpGet(uint16_t port, const std::string& path) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }
  const std::string req = "GET " + path + " HTTP/1.0\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string resp;
  char buf[2048];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  const size_t body = resp.find("\r\n\r\n");
  return body == std::string::npos ? std::string{} : resp.substr(body + 4);
}

bool WaitUntil(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

TEST(ThinServer, KernelToHttpQueryLoop) {
  ThinServer server;
  std::string err;
  ASSERT_TRUE(server.Start({0, 0, 1024}, &err));

  // —— 端侧内核：真实线程壳 + TCP 通道 + 日志域 ——
  const std::string dir = TmpDir();
  TcpChannel::Options copt;
  copt.port = server.ingest_port();
  auto channel = std::make_unique<TcpChannel>(copt);
  SchemaRegistry schema;
  const uint64_t k_send = schema.RegisterCounter("msg.send.total", {});
  const uint64_t k_rtt = schema.RegisterHistogram("rpc.rtt.ms", {});
  ASSERT_TRUE(k_send != 0 && k_rtt != 0);

  KernelOptions opt;
  opt.control_key = "test-only-control-key-not-for-production";
  opt.poll_interval_ms = 1;
  opt.trace_chunk_max_age_ms = 20;
  opt.metrics_snapshot_interval_ms = 50;   // 快照快跑：e2e 不等 30s
  opt.pipeline.p1_flush_bytes = 1;
  opt.pipeline.p2_flush_interval_ms = 50;  // P2（指标）同样快跑：默认 5min 是生产节奏
  for (auto& w : opt.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  opt.log_path = dir + "/obs.log";
  auto kernel = ObsKernel::Start(dir, channel.get(), &schema, opt, &err);
  ASSERT_TRUE(kernel != nullptr);

  // 四类信号：指标（计数 + 直方图）、轨迹（T0/T2/T4）、日志 + 回捞命令。
  kernel->AddCounter(k_send, 0, 2);
  for (const uint64_t v : {10, 20, 30}) kernel->RecordHistogram(k_rtt, 0, v);
  for (const uint8_t st : {0, 2, 4}) kernel->TraceStage(4242, st, 0);
  kernel->Log(2, imobs::Fnv1a("imlink"), "fetch-me conn reset");
  kernel->SetFetchConsent(true);

  Command fetch;
  fetch.command_id = 7;
  fetch.type = CommandType::kFetchLogs;
  fetch.arg_a = 0;
  fetch.arg_b = static_cast<uint64_t>(imobs::RealClock::Instance().WallMillis()) + 60'000;
  std::vector<uint8_t> payload;
  EncodeCommand(fetch, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopeCommand, payload, "test-only-control-key-not-for-production", &env);
  kernel->SubmitControlEnvelope(std::string_view(
      reinterpret_cast<const char*>(env.data()), env.size()));

  // 等聚合齐活：旅程 + 指标 + 回捞产物（消费线程 + TCP + 服务端三段异步）。
  // 分腿等待：哪一路信号缺席，失败行直接指认。
  ASSERT_TRUE(WaitUntil([&] { return server.collector().JourneyCount() >= 1; }, 8'000));
  ASSERT_TRUE(WaitUntil([&] { return !server.MetricsSnapshot().empty(); }, 8'000));
  ASSERT_TRUE(WaitUntil([&] { return !server.LogsOf(7).empty(); }, 8'000));

  // —— HTTP 查询面断言 ——
  const uint16_t hp = server.http_port();
  const std::string health = HttpGet(hp, "/healthz");
  EXPECT_TRUE(health.find("\"ok\":true") != std::string::npos);
  EXPECT_TRUE(health.find("\"journeys\":1") != std::string::npos);

  const std::string journey = HttpGet(hp, "/journey?msg=4242");
  EXPECT_TRUE(journey.find("\"stages\":[0,2,4]") != std::string::npos);

  const std::string metrics = HttpGet(hp, "/metrics");
  EXPECT_TRUE(metrics.find("\"key\":" + std::to_string(k_send)) != std::string::npos);
  EXPECT_TRUE(metrics.find("\"key\":" + std::to_string(k_rtt)) != std::string::npos);
  EXPECT_TRUE(metrics.find("\"p99\":") != std::string::npos);  // t-digest 分位在线

  const std::string logs = HttpGet(hp, "/logs?cmd=7");
  EXPECT_TRUE(logs.find("fetch-me conn reset") != std::string::npos);

  const std::string missing = HttpGet(hp, "/journey?msg=999");
  EXPECT_TRUE(missing.find("not-found") != std::string::npos);

  // 程序化查询面与 HTTP 同源。
  const auto rows = server.MetricsSnapshot();
  bool saw_counter = false;
  for (const auto& r : rows) {
    if (r.key == k_send) {
      EXPECT_EQ(r.sum, 2u);  // counter 语义：sum = 累计增量（count = 事件次数）
      saw_counter = true;
    }
  }
  EXPECT_TRUE(saw_counter);

  kernel->Stop();
  kernel.reset();
  channel.reset();
  server.Stop();
}

}  // namespace

IMOBS_TEST_MAIN
