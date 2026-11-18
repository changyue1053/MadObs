// imobs echo 演示台（实施计划 P8）：一条命令演完整版故事。
//
// 单进程装配：薄服务端 + 三个真实内核（发送端 A / 服务侧 SVC / 接收端 B），
// 各自独立 TcpChannel 经真实 socket 上报——与生产的区别只是进程边界。
//
// 演示剧本：
//   1. 5 条消息全链路物流单：A 打 T0/T1/T2/T3，SVC 打 T4/T5，B 打 T6/T8/T9；
//      其中 msg=9005 故意让 B"失联"（不打 T6/T8/T9）→ 查询面直接看断链于 T5→T6。
//   2. 指标：A 记发送计数与 RTT 直方图（t-digest 分位在服务端合并后在线可查）。
//   3. 自愈：B 触发"连接假死"信号 → 防抖 → 诊断 → 重连动作 → 'H' 事件上行。
//   4. 回捞：A 记结构化日志，用户授权后服务端下发 kFetchLogs → 'L' 分片上行对账。
//
// 用法：imobs_echo_demo [--serve]   （--serve = 演完不退出，供浏览器/curl 探索）
// 退出码：0 = 剧本全部验收通过（因此它同时是 P8 的冒烟测试）。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imobs/clock.h"
#include "imobs/obs_kernel.h"
#include "imobs/policy.h"
#include "imobs/tcp_channel.h"
#include "imobs/thin_server.h"

using imobs::ActionSpec;
using imobs::Command;
using imobs::CommandType;
using imobs::EncodeCommand;
using imobs::Fnv1a;
using imobs::IActionExecutor;
using imobs::kEnvelopeCommand;
using imobs::KernelOptions;
using imobs::ObsKernel;
using imobs::SceneSpec;
using imobs::SchemaRegistry;
using imobs::SignEnvelope;
using imobs::TcpChannel;
using imobs::ThinServer;

namespace {

std::string TmpDir(const char* tag) {
  std::string tmpl = std::string("/tmp/imobs_demo_") + tag + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  return std::string(mkdtemp(buf.data()));
}

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
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return pred();
}

// 演示端节点：内核 + 通道 + schema（声明顺序即析构契约：kernel 先于 channel 析构）。
struct Node {
  std::unique_ptr<TcpChannel> channel;
  SchemaRegistry schema;
  std::unique_ptr<ObsKernel> kernel;
  uint64_t k_send = 0;
  uint64_t k_rtt = 0;
};

std::unique_ptr<Node> StartNode(const char* tag, uint16_t ingest_port, bool with_logs,
                                std::vector<std::pair<SceneSpec, IActionExecutor*>> scenes = {}) {
  auto n = std::make_unique<Node>();
  TcpChannel::Options copt;
  copt.port = ingest_port;
  n->channel = std::make_unique<TcpChannel>(copt);
  n->k_send = n->schema.RegisterCounter("msg.send.total", {});
  n->k_rtt = n->schema.RegisterHistogram("rpc.rtt.ms", {});

  KernelOptions opt;
  opt.control_key = "test-only-control-key-not-for-production";
  opt.poll_interval_ms = 1;
  opt.trace_chunk_max_age_ms = 20;
  opt.metrics_snapshot_interval_ms = 200;
  opt.pipeline.p1_flush_bytes = 1;
  opt.pipeline.p2_flush_interval_ms = 200;
  for (auto& w : opt.pipeline.wal) {
    w.segment_size = 8192;
    w.segment_count = 2;
  }
  const std::string dir = TmpDir(tag);
  if (with_logs) opt.log_path = dir + "/obs.log";
  opt.heal_scenes = std::move(scenes);
  std::string err;
  n->kernel = ObsKernel::Start(dir, n->channel.get(), &n->schema, opt, &err);
  if (!n->kernel) {
    std::fprintf(stderr, "节点 %s 启动失败: %s\n", tag, err.c_str());
    return nullptr;
  }
  return n;
}

// 自愈演示执行体：重连动作，立即成功（真实域里这是 imlink 的 Reconnect）。
struct DemoReconnect final : IActionExecutor {
  void Execute(uint64_t, const std::string& action, DoneFn done) override {
    std::printf("    [自愈] 执行动作 %s → 成功\n", action.c_str());
    done(true);
  }
  void Verify(uint64_t, DoneFn done) override { done(true); }
};

void Show(uint16_t hp, const std::string& path) {
  std::printf("  GET %-28s %s\n", path.c_str(), HttpGet(hp, path).c_str());
}

int Fail(const char* what) {
  std::fprintf(stderr, "[demo] 验收失败：%s\n", what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const bool serve = argc > 1 && std::strcmp(argv[1], "--serve") == 0;
  std::printf("== imobs echo 演示台：薄服务端 + 三内核（发送端/服务侧/接收端） ==\n");

  ThinServer server;
  std::string err;
  if (!server.Start({0, 0, 4096}, &err)) {
    std::fprintf(stderr, "薄服务端启动失败: %s\n", err.c_str());
    return 1;
  }
  const uint16_t hp = server.http_port();
  std::printf("薄服务端: ingest=127.0.0.1:%u  http=http://127.0.0.1:%u\n\n", server.ingest_port(),
              hp);

  // 接收端 B 带自愈场景：信号 0x51（连接假死）→ 防抖 300ms → 诊断 200ms → 重连。
  DemoReconnect reconnect;
  SceneSpec scene;
  scene.scene_id = Fnv1a("scene.conn.dead");
  scene.detector.all_of = {0x51};
  scene.detector.debounce_ms = 300;
  scene.detector.debounce_jitter_ms = 0;
  scene.detector.diagnose_ms = 200;
  ActionSpec act;
  act.name = "reconnect";
  act.timeout_ms = 2'000;
  scene.ladder = {act};

  auto a = StartNode("sender", server.ingest_port(), /*with_logs=*/true);
  auto svc = StartNode("svc", server.ingest_port(), /*with_logs=*/false);
  auto b = StartNode("receiver", server.ingest_port(), /*with_logs=*/false, {{scene, &reconnect}});
  if (!a || !svc || !b) return 1;

  // —— 剧本 1：5 条消息全链路（msg=9005 接收端失联 → 断链演示）——
  std::printf("[1] 5 条消息全链路物流单（9005 故意断链于 T5→T6）\n");
  for (uint64_t msg = 9001; msg <= 9005; ++msg) {
    for (const uint8_t st : {0, 1, 2, 3}) a->kernel->TraceStage(msg, st, 0);
    a->kernel->AddCounter(a->k_send, 0, 1);
    a->kernel->RecordHistogram(a->k_rtt, 0, 40 + (msg % 5) * 17);
    for (const uint8_t st : {4, 5}) svc->kernel->TraceStage(msg, st, 0);
    if (msg != 9005) {
      for (const uint8_t st : {6, 8, 9}) b->kernel->TraceStage(msg, st, 0);
    }
  }

  // —— 剧本 2：自愈（B 的连接假死信号 → MAPE 环 → 'H' 事件上行）——
  std::printf("[2] 接收端连接假死 → 防抖 → 诊断 → 重连 → 自愈事件上行\n");
  b->kernel->SetHealthSignal(0x51, true);

  // —— 剧本 3：日志 + 回捞（授权 → 服务端命令 → 'L' 分片对账）——
  std::printf("[3] 发送端记日志 → 用户授权 → kFetchLogs 命令 → 回捞上行\n");
  a->kernel->Log(2, Fnv1a("imlink"), "demo: conn reset, retry in 2s");
  a->kernel->Log(3, Fnv1a("imdb"), "demo: sql slow 812ms");
  a->kernel->SetFetchConsent(true);
  Command fetch;
  fetch.command_id = 42;
  fetch.type = CommandType::kFetchLogs;
  fetch.arg_a = 0;
  fetch.arg_b = static_cast<uint64_t>(imobs::RealClock::Instance().WallMillis()) + 60'000;
  std::vector<uint8_t> payload;
  EncodeCommand(fetch, &payload);
  std::vector<uint8_t> env;
  SignEnvelope(kEnvelopeCommand, payload, "test-only-control-key-not-for-production", &env);
  a->kernel->SubmitControlEnvelope(
      std::string_view(reinterpret_cast<const char*>(env.data()), env.size()));

  // —— 等三段异步全落定，随后逐项验收 ——
  std::printf("\n等待上报落定（三段异步：消费线程 → TCP → 服务端聚合）...\n\n");
  const bool settled = WaitUntil(
      [&] {
        if (server.collector().JourneyCount() < 5) return false;
        if (server.LogsOf(42).empty()) return false;
        bool metrics_ok = false;
        for (const auto& r : server.MetricsSnapshot()) {
          if (r.key == a->k_rtt && r.count >= 5) metrics_ok = true;
        }
        // 自愈要等到"动作已执行"（防抖 300ms + 诊断 200ms 之后），不止步于状态迁移。
        const std::string heals = HttpGet(hp, "/heals");
        return metrics_ok && heals.find("\"action\":\"reconnect\"") != std::string::npos;
      },
      10'000);
  if (!settled) return Fail("信号未在期限内落定");

  std::printf("—— 查询面（排障者视角）——\n");
  Show(hp, "/healthz");
  Show(hp, "/journey?msg=9001");
  Show(hp, "/journey?msg=9005");
  Show(hp, "/metrics");
  Show(hp, "/heals");
  Show(hp, "/logs?cmd=42");

  // —— 冒烟验收（P8 DoD：演示台本身就是回归资产）——
  if (HttpGet(hp, "/journey?msg=9001").find("\"stages\":[0,1,2,3,4,5,6,8,9]") == std::string::npos)
    return Fail("9001 全链路不完整");
  if (HttpGet(hp, "/journey?msg=9005").find("\"stages\":[0,1,2,3,4,5]") == std::string::npos)
    return Fail("9005 断链形状不对（应止步 T5）");
  if (HttpGet(hp, "/metrics").find("\"p99\":") == std::string::npos)
    return Fail("t-digest 分位缺席");
  if (HttpGet(hp, "/logs?cmd=42").find("sql slow 812ms") == std::string::npos)
    return Fail("回捞产物缺席");
  if (HttpGet(hp, "/heals").find("\"action\":\"reconnect\"") == std::string::npos)
    return Fail("自愈动作记录缺席");

  std::printf("\n== 剧本全部验收通过 ==\n");
  std::printf("断链读法：9005 止步 T5（服务端已投递）→ 责任段 = T5→T6（推送触达），\n");
  std::printf("          与 9001 的完整物流单对照即得结论——这就是分段归因。\n");

  if (serve) {
    std::printf("\n--serve 模式：演示台保持运行（Ctrl-C 退出），试试：\n");
    std::printf("  curl http://127.0.0.1:%u/journeys\n", hp);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  b->kernel->Stop();
  svc->kernel->Stop();
  a->kernel->Stop();
  b.reset();
  svc.reset();
  a.reset();
  server.Stop();
  return 0;
}
