// 崩溃剧本（实施计划 P2.2 DoD）：fork 子进程写入后 _exit —— 不跑析构、不 msync，
// 等价 kill -9。验证口径：零丢已完成记录、零半条记录、水位存活。
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "imobs/wal_ring.h"
#include "imobs_test.h"

using imobs::WalOptions;
using imobs::WalRing;

namespace {

std::string TmpWalPath(const char* tag) {
  char tmpl[] = "/tmp/imobs_crash_XXXXXX";
  char* dir = mkdtemp(tmpl);
  return std::string(dir) + "/" + tag + ".wal";
}

WalOptions SmallOpt() {
  WalOptions opt;
  opt.segment_size = 4096;
  opt.segment_count = 4;
  return opt;
}

}  // namespace

TEST(WalRecovery, Kill9LosesNothing) {
  const std::string path = TmpWalPath("kill9");
  const pid_t pid = fork();
  ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    std::string err;
    auto w = WalRing::Open(path, SmallOpt(), &err);
    if (!w) _exit(2);
    for (int i = 0; i < 10; ++i) {
      char b[32];
      const int n = std::snprintf(b, sizeof(b), "crash-rec-%02d", i);
      if (!w->Append(b, static_cast<uint32_t>(n))) _exit(3);
    }
    _exit(0);  // 绕过析构与 msync：脏页由内核回写，这就是 kill -9 的语义
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  std::string err;
  auto w = WalRing::Open(path, SmallOpt(), &err);
  ASSERT_TRUE(w != nullptr);
  std::vector<std::string> got;
  w->ScanPending([&](const WalRing::Record& r) {
    got.emplace_back(r.payload);
    return true;
  });
  ASSERT_EQ(got.size(), size_t{10});
  EXPECT_TRUE(got.front() == std::string("crash-rec-00"));
  EXPECT_TRUE(got.back() == std::string("crash-rec-09"));
  EXPECT_EQ(w->stats().torn_dropped, 0u);
}

TEST(WalRecovery, WatermarkSurvivesKill9) {
  const std::string path = TmpWalPath("kill9wm");
  const pid_t pid = fork();
  ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    std::string err;
    auto w = WalRing::Open(path, SmallOpt(), &err);
    if (!w) _exit(2);
    std::vector<WalRing::Cursor> ends;
    for (int i = 0; i < 5; ++i) {
      char b[16];
      const int n = std::snprintf(b, sizeof(b), "w%d", i);
      if (!w->Append(b, static_cast<uint32_t>(n))) _exit(3);
    }
    w->ScanPending([&](const WalRing::Record& r) {
      ends.push_back(r.end);
      return true;
    });
    if (ends.size() != 5 || !w->MarkConsumed(ends[2])) _exit(4);
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  std::string err;
  auto w = WalRing::Open(path, SmallOpt(), &err);
  ASSERT_TRUE(w != nullptr);
  std::vector<std::string> got;
  w->ScanPending([&](const WalRing::Record& r) {
    got.emplace_back(r.payload);
    return true;
  });
  ASSERT_EQ(got.size(), size_t{2});  // 已消费的 w0~w2 不再补报：补报幂等的地基
  EXPECT_TRUE(got[0] == std::string("w3"));
  EXPECT_TRUE(got[1] == std::string("w4"));
}

IMOBS_TEST_MAIN
