// IMDB(Rust) × imobs(C++) 宿主桥 e2e —— 三内核集成的"另一半"（imlink 已在 M2 证毕）：
// imdb-ffi 静态库真实链入本测试，imdb_open_observed 的观测回调（写 Actor 线程、
// COMMIT 之后）桥到 imobs 热路径打点，经真实 TCP 上报，在薄服务端 HTTP 查询面验收。
//
// 桥接契约（生产宿主适配层的参考实现，架构 §6 双 ID 桥接规则）：
//   T1（stage=1，只有 client_msg_id）→ msgTrace 实体 = Fnv1a(client_msg_id)
//   T8（stage=8，双 ID 齐备）        → msgTrace 实体 = 服务端 msg_id
//   db.commit_ms / db.commit_batch_jobs → 直方图（《指标字典》存储 SLI）
//
// 线程语义（这正是本测试的价值）：回调发生在 imdb 的写 Actor 线程——一个 imobs
// 完全不认识的外来线程；打点 API 的 wait-free 契约在此接受真实跨语言检验。
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imdb.h"
#include "imobs/clock.h"
#include "imobs/obs_kernel.h"
#include "imobs/schema.h"
#include "imobs/tcp_channel.h"
#include "imobs/thin_server.h"
#include "imobs_test.h"

using imobs::Fnv1a;
using imobs::KernelOptions;
using imobs::ObsKernel;
using imobs::SchemaRegistry;
using imobs::TcpChannel;
using imobs::ThinServer;

namespace {

std::string TmpDir(const char* tag) {
  std::string tmpl = std::string("/tmp/imobs_") + tag + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  return std::string(mkdtemp(buf.data()));
}

// 宿主桥：imdb 观测回调 → imobs 打点（回调在写 Actor 线程，打点 wait-free）。
struct Bridge {
  ObsKernel* kernel = nullptr;
  uint64_t k_commit_ms = 0;
  uint64_t k_batch_jobs = 0;
};

extern "C" void OnImdbTrace(void* ud, int32_t stage, int64_t msg_id,
                            const char* client_msg_id) {
  auto* b = static_cast<Bridge*>(ud);
  if (stage == 1) {
    // T1：服务端 msg_id 尚未发号，实体 = client_msg_id 哈希（T4 处靠双 ID 焊接）。
    b->kernel->TraceStage(Fnv1a(client_msg_id), /*stage=*/1, /*status=*/0);
  } else if (stage == 8 && msg_id >= 0) {
    b->kernel->TraceStage(static_cast<uint64_t>(msg_id), /*stage=*/8, /*status=*/0);
  }
}

extern "C" void OnImdbMetric(void* ud, const char* key, int64_t value) {
  auto* b = static_cast<Bridge*>(ud);
  const std::string_view k(key);
  if (k == "db.commit_ms") {
    b->kernel->RecordHistogram(b->k_commit_ms, 0, static_cast<uint64_t>(value));
  } else if (k == "db.commit_batch_jobs") {
    b->kernel->RecordHistogram(b->k_batch_jobs, 0, static_cast<uint64_t>(value));
  }
}

bool WaitUntil(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

std::string HttpGet(uint16_t port, const std::string& path);  // 定义在文件尾（socket 样板）

TEST(ImdbBridge, RustKernelStampsFlowToThinServer) {
  ThinServer server;
  std::string err;
  ASSERT_TRUE(server.Start({0, 0, 1024}, &err));

  // —— imobs 内核（真实线程壳 + TCP 通道）——
  TcpChannel::Options copt;
  copt.port = server.ingest_port();
  auto channel = std::make_unique<TcpChannel>(copt);
  SchemaRegistry schema;
  Bridge bridge;
  bridge.k_commit_ms = schema.RegisterHistogram("db.commit_ms", {});
  bridge.k_batch_jobs = schema.RegisterHistogram("db.commit_batch_jobs", {});
  ASSERT_TRUE(bridge.k_commit_ms != 0 && bridge.k_batch_jobs != 0);

  KernelOptions opt;
  opt.poll_interval_ms = 1;
  opt.trace_chunk_max_age_ms = 20;
  opt.metrics_snapshot_interval_ms = 50;
  opt.pipeline.p1_flush_bytes = 1;
  opt.pipeline.p2_flush_interval_ms = 50;
  for (auto& w : opt.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  auto kernel = ObsKernel::Start(TmpDir("bridge_obs"), channel.get(), &schema, opt, &err);
  ASSERT_TRUE(kernel != nullptr);
  bridge.kernel = kernel.get();

  // —— IMDB（Rust 内核，经 C ABI 打开并接上观测桥）——
  const std::string db_dir = TmpDir("bridge_db");
  uint64_t db = 0;
  ASSERT_EQ(imdb_open_observed(db_dir.c_str(), "alice", &OnImdbTrace, &OnImdbMetric, &bridge, &db),
            0);
  ASSERT_TRUE(db != 0);

  // 发送链：outbox 落库 → T1（client_msg_id 哈希为实体）。
  int64_t rowid = -1;
  ASSERT_EQ(imdb_outbox_enqueue(db, "c1", "cm-out-1", 0, "me: hello", nullptr, nullptr, 0, &rowid),
            0);

  // 接收链：一批两条远端消息 → 两个 T8（服务端 msg_id 为实体）。
  ImdbIncoming msgs[2] = {};
  msgs[0].conv_id = "c1";
  msgs[0].seq = 1;
  msgs[0].client_msg_id = "cm-r-1";
  msgs[0].msg_id = 1000;
  msgs[0].sender_id = "bob";
  msgs[0].digest = "bob: 1";
  msgs[0].server_time = 2'000'001;
  msgs[1] = msgs[0];
  msgs[1].seq = 2;
  msgs[1].client_msg_id = "cm-r-2";
  msgs[1].msg_id = 2000;
  msgs[1].digest = "bob: 2";
  msgs[1].server_time = 2'000'002;
  ImdbApplyReport report{};
  ASSERT_EQ(imdb_apply_incoming(db, msgs, 2, &report), 0);
  EXPECT_EQ(report.applied, 2u);

  // close 阻塞至写 Actor 退出：全部观测吐出已完成（回调时序的收口保证）。
  ASSERT_EQ(imdb_close(db), 0);

  // —— 薄服务端验收：三条旅程 + 存储 SLI 直方图 ——
  ASSERT_TRUE(WaitUntil([&] { return server.collector().JourneyCount() >= 3; }, 8'000));
  ASSERT_TRUE(WaitUntil(
      [&] {
        for (const auto& r : server.MetricsSnapshot()) {
          if (r.key == bridge.k_commit_ms && r.count >= 2) return true;  // 两次提交
        }
        return false;
      },
      8'000));

  const uint16_t hp = server.http_port();
  const std::string j_t8 = HttpGet(hp, "/journey?msg=1000");
  EXPECT_TRUE(j_t8.find("\"stages\":[8]") != std::string::npos);
  const std::string j_t1 =
      HttpGet(hp, "/journey?msg=" + std::to_string(Fnv1a("cm-out-1")));
  EXPECT_TRUE(j_t1.find("\"stages\":[1]") != std::string::npos);
  const std::string metrics = HttpGet(hp, "/metrics");
  EXPECT_TRUE(metrics.find("\"key\":" + std::to_string(bridge.k_commit_ms)) != std::string::npos);

  kernel->Stop();
  kernel.reset();
  channel.reset();
  server.Stop();
}

}  // namespace

// —— socket 样板（与 test_thin_server 同式；文件内自足，避免测试间共享头）——
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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

}  // namespace

IMOBS_TEST_MAIN
