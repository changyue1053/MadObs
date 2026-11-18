#include "imobs/crash_sentry.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <ctime>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace imobs {
namespace {

constexpr uint32_t kMagic = 0x494D4352;  // 'IMCR'
constexpr size_t kFileSize = 4096;
constexpr size_t kRingOff = 64;
constexpr size_t kRingSlots = 8;
constexpr size_t kSeqOff = 128;
constexpr size_t kFrameCountOff = 132;
constexpr size_t kSlideOff = 136;
constexpr size_t kFramesOff = 144;
constexpr size_t kMaxFrames = 32;

uint8_t* g_map = nullptr;  // 进程级：signal handler 无上下文可带

const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

// —— 帧指针链回溯（async-signal-safe：只读栈内存 + 纯算术）——
// 校验三件套压低"链损坏时再崩"的概率：16B 对齐、单调向栈底、步进 ≤1MB、
// 总深度 ≤32。中途再崩的兜底在调用侧：帧计数逐帧落位，坏链只丢尾部帧。
struct RawFrame {
  uint64_t fp;
  uint64_t pc;
};

bool NextFrame(uint64_t fp, RawFrame* out) {
  if (fp == 0 || (fp & 0xF) != 0) return false;
  const auto* slot = reinterpret_cast<const uint64_t*>(fp);
  out->fp = slot[0];   // AAPCS64 / x86_64 SysV：[fp] = 上一帧 fp
  out->pc = slot[1];   // [fp+8] = 返回地址
  if (out->fp != 0) {
    if (out->fp <= fp) return false;             // 必须单调向栈底
    if (out->fp - fp > 1024 * 1024) return false;  // 单帧 >1MB 视为链损坏
  }
  return true;
}

// 从 ucontext 提取崩溃现场三元组（pc / lr / fp）；不支持的平台返回 false。
bool CrashRegs(void* uctx, uint64_t* pc, uint64_t* lr, uint64_t* fp) {
#if defined(__APPLE__) && defined(__aarch64__)
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  if (uc == nullptr || uc->uc_mcontext == nullptr) return false;
  *pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
  *lr = __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
  *fp = __darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
  return true;
#elif defined(__APPLE__) && defined(__x86_64__)
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  if (uc == nullptr || uc->uc_mcontext == nullptr) return false;
  *pc = uc->uc_mcontext->__ss.__rip;
  *lr = 0;
  *fp = uc->uc_mcontext->__ss.__rbp;
  return true;
#elif defined(__linux__) && defined(__aarch64__)
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  *pc = uc->uc_mcontext.pc;
  *lr = uc->uc_mcontext.regs[30];
  *fp = uc->uc_mcontext.regs[29];
  return true;
#elif defined(__linux__) && defined(__x86_64__)
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  *pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  *lr = 0;
  *fp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
  return true;
#else
  (void)uctx;
  (void)pc;
  (void)lr;
  (void)fp;
  return false;
#endif
}

uint8_t* MapFile(const std::string& path, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (err) *err = "open failed";
    return nullptr;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || (st.st_size != 0 && st.st_size != kFileSize)) {
    if (st.st_size != 0) {
      ::close(fd);
      if (err) *err = "size mismatch";
      return nullptr;
    }
  }
  if (st.st_size == 0 && ::ftruncate(fd, kFileSize) != 0) {
    ::close(fd);
    if (err) *err = "ftruncate failed";
    return nullptr;
  }
  void* m = ::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);  // mmap 持有引用，fd 可即刻关闭
  if (m == MAP_FAILED) {
    if (err) *err = "mmap failed";
    return nullptr;
  }
  return static_cast<uint8_t*>(m);
}

// async-signal-safe：只有对已映射内存的纯写 + 栈读 + time(2)（POSIX 安全清单内）。
// 本函数及其调用链受 scripts/audit_signal_safe.py 白名单审计（CI 红灯，清单外调用即失败）。
void OnFatalSignal(int sig, siginfo_t* info, void* uctx) {
  if (g_map != nullptr) {
    const int64_t now_ms = static_cast<int64_t>(::time(nullptr)) * 1000;
    // 先记历史环再记待补报标记：标记写到一半再崩，loop 证据也已留下
    auto* ring = reinterpret_cast<int64_t*>(g_map + kRingOff);
    auto* seqp = reinterpret_cast<uint32_t*>(g_map + kSeqOff);
    const uint32_t seq = *seqp + 1;
    ring[seq % kRingSlots] = now_ms;
    *seqp = seq;
    auto* marker = reinterpret_cast<CrashMarker*>(g_map);
    marker->signal_no = sig;
    marker->wall_ms = now_ms;
    marker->fault_addr =
        info != nullptr ? reinterpret_cast<uint64_t>(info->si_addr) : 0;
    marker->seq = seq;
    marker->magic = kMagic;  // magic 最后落位：读侧见 magic 即整条有效

    // 回溯严格在 marker 之后：帧计数逐帧落位——走坏链再崩也只丢尾部帧，
    // 核心 marker 与 crash-loop 证据已持久。
    auto* nframes = reinterpret_cast<uint32_t*>(g_map + kFrameCountOff);
    auto* pcs = reinterpret_cast<uint64_t*>(g_map + kFramesOff);
    *nframes = 0;
    uint64_t pc = 0;
    uint64_t lr = 0;
    uint64_t fp = 0;
    if (CrashRegs(uctx, &pc, &lr, &fp)) {
      uint32_t n = 0;
      pcs[n++] = pc;  // 第 0 帧：崩溃指令本身
      *nframes = n;
      if (lr != 0 && n < kMaxFrames) {
        pcs[n++] = lr;  // arm64 叶函数：返回地址还在 lr、未压栈
        *nframes = n;
      }
      RawFrame f{fp, 0};
      while (n < kMaxFrames && f.fp != 0) {
        RawFrame next{};
        if (!NextFrame(f.fp, &next)) break;
        if (next.pc < 4096) break;  // 零页/哨兵：链走完
        pcs[n++] = next.pc;
        *nframes = n;  // 逐帧递增：任何一步再崩，前缀仍有效
        f = next;
      }
    }
  }
  ::signal(sig, SIG_DFL);
  ::raise(sig);  // 交还默认处置：core dump / 终止语义不被观测吞掉
}

}  // namespace

bool CrashSentry::Install(const Options& opt, std::string* err) {
  if (g_map != nullptr) {
    if (err) *err = "already installed";
    return false;
  }
  uint8_t* m = MapFile(opt.path, err);
  if (m == nullptr) return false;
  g_map = m;

  // 主镜像 slide 预取（handler 内禁 dyld 调用）：离线符号化 = pc - slide。
  uint64_t slide = 0;
#if defined(__APPLE__)
  slide = static_cast<uint64_t>(_dyld_get_image_vmaddr_slide(0));
#endif
  std::memcpy(m + kSlideOff, &slide, sizeof(slide));

  struct sigaction sa {};
  sa.sa_sigaction = &OnFatalSignal;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  for (const int sig : kSignals) {
    if (::sigaction(sig, &sa, nullptr) != 0) {
      if (err) *err = "sigaction failed";
      return false;
    }
  }
  return true;
}

bool CrashSentry::TakePendingMarker(const std::string& path, CrashMarker* out,
                                    CrashBacktrace* bt) {
  uint8_t* m = MapFile(path, nullptr);
  if (m == nullptr) return false;
  auto* marker = reinterpret_cast<CrashMarker*>(m);
  bool has = marker->magic == kMagic;
  if (has) {
    *out = *marker;
    if (bt != nullptr) {
      uint32_t n = 0;
      std::memcpy(&n, m + kFrameCountOff, sizeof(n));
      bt->n_frames = n <= kMaxFrames ? n : 0;  // 越界 = 页损坏：宁可置空
      std::memcpy(&bt->image_slide, m + kSlideOff, sizeof(bt->image_slide));
      std::memcpy(bt->pc, m + kFramesOff, sizeof(uint64_t) * kMaxFrames);
    }
    marker->magic = 0;  // 取出即清除：补报幂等（重复启动不重复补报）
    std::memset(m + kFrameCountOff, 0, sizeof(uint32_t));
    ::msync(m, kFileSize, MS_ASYNC);
  }
  ::munmap(m, kFileSize);
  return has;
}

bool CrashSentry::InCrashLoop(const std::string& path, uint32_t crashes_threshold,
                              int64_t window_ms, int64_t now_wall_ms) {
  uint8_t* m = MapFile(path, nullptr);
  if (m == nullptr) return false;
  const auto* ring = reinterpret_cast<const int64_t*>(m + kRingOff);
  uint32_t hits = 0;
  for (size_t i = 0; i < kRingSlots; ++i) {
    const int64_t ts = ring[i];
    if (ts > 0 && ts <= now_wall_ms && now_wall_ms - ts <= window_ms) ++hits;
  }
  ::munmap(m, kFileSize);
  return hits >= crashes_threshold;
}

}  // namespace imobs
