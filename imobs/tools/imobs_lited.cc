// imobs-lite 守护进程：薄服务端的可运行形态。
//   ./imobs_lited [ingest_port] [http_port]   （缺省 9500 / 9580）
// ingest 收端侧 TcpChannel 的批（幂等 + 拼接 + 聚合），HTTP 提供只读 JSON 查询。
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imobs/thin_server.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void OnSig(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
  const uint16_t ingest = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9500;
  const uint16_t http = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 9580;

  imobs::ThinServer server;
  std::string err;
  if (!server.Start({ingest, http, 4096}, &err)) {
    std::fprintf(stderr, "imobs_lited: start failed: %s\n", err.c_str());
    return 1;
  }
  std::printf("imobs_lited: ingest=127.0.0.1:%u http=http://127.0.0.1:%u\n",
              server.ingest_port(), server.http_port());
  std::printf("  GET /healthz /metrics /journeys /journey?msg=<id> /heals /events /logs?cmd=<id>\n");

  std::signal(SIGINT, &OnSig);
  std::signal(SIGTERM, &OnSig);
  while (g_stop == 0) {
    struct timespec ts {0, 100'000'000};
    nanosleep(&ts, nullptr);
  }
  server.Stop();
  std::printf("imobs_lited: bye\n");
  return 0;
}
