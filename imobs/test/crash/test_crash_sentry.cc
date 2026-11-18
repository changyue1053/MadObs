// 崩溃哨兵剧本（实施计划 P5.3/P5.4 DoD）：
// fork 子进程安装哨兵后以致命信号自杀——handler 只做 mmap 纯写 + re-raise；
// 父进程验证：标记可补报且取出即清、crash-loop 窗口判定、默认处置未被吞。
// 注：用 SIGABRT 触发（SIGSEGV/SIGBUS 会与 ASan 自身的拦截纠缠，属测试环境约束）。
#include "imobs/crash_sentry.h"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <ctime>
#include <string>

#include "imobs_test.h"

using imobs::CrashBacktrace;
using imobs::CrashMarker;
using imobs::CrashSentry;

namespace {

std::string TmpCrashPath() {
  char tmpl[] = "/tmp/imobs_crash_XXXXXX";
  char* dir = mkdtemp(tmpl);
  return std::string(dir) + "/crash.bin";
}

// 已具名调用链（禁内联 + 汇编屏障防尾调优化）：回溯断言的已知形状。
__attribute__((noinline)) void CrashLevel3() {
  __builtin_trap();  // synchronous fault preserves our own frame chain, unlike libc raise
  asm volatile("");
}
__attribute__((noinline)) void CrashLevel2() {
  CrashLevel3();
  asm volatile("");
}
__attribute__((noinline)) void CrashLevel1() {
  CrashLevel2();
  asm volatile("");
}

// 子进程：装哨兵 → 自杀。返回 true = 子进程确实死于该信号（默认处置未被吞）。
bool CrashChild(const std::string& path, int sig) {
  const pid_t pid = fork();
  if (pid == 0) {
    std::string err;
    if (!CrashSentry::Install({path}, &err)) _exit(9);
    ::raise(sig);
    _exit(8);  // 不应到达：handler re-raise 默认处置必须杀死进程
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) return false;
  return WIFSIGNALED(status) && WTERMSIG(status) == sig;
}

}  // namespace

TEST(CrashSentry, MarkerWrittenOnceAndCleared) {
  const std::string path = TmpCrashPath();
  ASSERT_TRUE(CrashChild(path, SIGABRT));

  CrashMarker m;
  ASSERT_TRUE(CrashSentry::TakePendingMarker(path, &m));
  EXPECT_EQ(m.signal_no, SIGABRT);
  EXPECT_TRUE(m.wall_ms > 0);
  EXPECT_EQ(m.seq, 1u);

  // 取出即清除：补报幂等，重复启动不重复补报
  CrashMarker again;
  EXPECT_FALSE(CrashSentry::TakePendingMarker(path, &again));
}

TEST(CrashSentry, BacktraceCapturedThroughNamedChain) {
  const std::string path = TmpCrashPath();
  const pid_t pid = fork();
  if (pid == 0) {
    std::string err;
    if (!CrashSentry::Install({path}, &err)) _exit(9);
    CrashLevel1();  // 三层已具名链 → raise(SIGILL)
    _exit(8);
  }
  int status = 0;
  ASSERT_TRUE(::waitpid(pid, &status, 0) == pid);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);

  CrashMarker m;
  CrashBacktrace bt;
  ASSERT_TRUE(CrashSentry::TakePendingMarker(path, &m, &bt));
  EXPECT_EQ(m.signal_no, SIGILL);
  // 至少捕到崩溃 PC + 数层外层帧（raise→Level3→Level2→Level1→CrashChild 主链；
  // 精确深度依平台 libc 帧形状，不做脆断言）。
  EXPECT_TRUE(bt.n_frames >= 3);
  for (uint32_t i = 0; i < bt.n_frames; ++i) {
    EXPECT_TRUE(bt.pc[i] >= 4096);  // 全部是可信地址（非零页）
  }
  // 取出即清除：回溯与 marker 同一生命周期。
  CrashBacktrace bt2;
  CrashMarker m2;
  EXPECT_FALSE(CrashSentry::TakePendingMarker(path, &m2, &bt2));
}

TEST(CrashSentry, CrashLoopDetectionWithinWindow) {
  const std::string path = TmpCrashPath();
  ASSERT_TRUE(CrashChild(path, SIGABRT));
  ASSERT_TRUE(CrashChild(path, SIGABRT));
  ASSERT_TRUE(CrashChild(path, SIGABRT));

  const int64_t now = static_cast<int64_t>(::time(nullptr)) * 1000;
  EXPECT_TRUE(CrashSentry::InCrashLoop(path, 3, 60'000, now));   // 60s 内 3 次：安全模式信号
  EXPECT_FALSE(CrashSentry::InCrashLoop(path, 4, 60'000, now));  // 阈值 4：未到
  // 窗口外不算：把"现在"推到一天后
  EXPECT_FALSE(CrashSentry::InCrashLoop(path, 3, 60'000, now + 86'400'000));
  // 从未崩溃过的路径：直接 false
  EXPECT_FALSE(CrashSentry::InCrashLoop(TmpCrashPath(), 1, 60'000, now));

  // 标记仍是最后一次崩溃（seq=3），历史环未被补报清除
  CrashMarker m;
  ASSERT_TRUE(CrashSentry::TakePendingMarker(path, &m));
  EXPECT_EQ(m.seq, 3u);
  EXPECT_TRUE(CrashSentry::InCrashLoop(path, 3, 60'000, now));  // 补报不清 loop 证据
}

IMOBS_TEST_MAIN
