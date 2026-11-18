#pragma once
// 崩溃哨兵（实施计划 P5.3/P5.4）：async-signal-safe 崩溃标记 + 次启补报 + crash-loop 判定。
//
// handler 铁律（架构 §10）：只做三件事——预分配 mmap 区的纯内存写、恢复默认处置、
// re-raise。禁 malloc / 锁 / IO / 任何 C++ 异常机制。持久化原理与 WAL 同源：
// 进程死亡不丢脏页（内核负责回写）；崩溃标记若逢掉电丢失 = 少一次补报，可接受。
//
// 文件布局（单页 4096B）：
//   [0, 32)    待补报标记 CrashMarker（次启 TakePendingMarker 取出并清除）
//   [64, 128)  最近 8 次崩溃 wall_ms 环（crash-loop 判定输入，永不清除）
//   [128, 132) 崩溃总序号 u32
//   [132, 136) 回溯帧数 u32（marker 落位后逐帧递增：走栈途中再崩只丢尾部帧）
//   [136, 144) 主镜像 slide u64（Install 时预取——handler 内禁 dyld 调用）
//   [144, 400) 回溯 PC × 32（u64；崩溃 PC 在前，向外层依次）

#include <cstdint>
#include <string>

namespace imobs {

struct CrashMarker {
  uint32_t magic = 0;  // 'IMCR'
  int32_t signal_no = 0;
  int64_t wall_ms = 0;
  uint64_t fault_addr = 0;
  uint32_t seq = 0;  // 本次崩溃的总序号（1 起）
};

// minidump-lite：帧指针链回溯（AAPCS64 / x86_64 帧链），配 slide 可离线符号化
// （file_addr = pc - slide，对 dSYM/objdump 直接可用）。
struct CrashBacktrace {
  uint32_t n_frames = 0;
  uint64_t image_slide = 0;
  uint64_t pc[32] = {};
};

class CrashSentry {
 public:
  struct Options {
    std::string path;
  };

  // 进程级安装（一次）：SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT。
  static bool Install(const Options& opt, std::string* err);

  // 次启检查：有待补报标记则取出并清除（P0 补报事件的输入）。
  // bt 可选：一并取出崩溃回溯（与 marker 同生命周期，同次清除）。
  static bool TakePendingMarker(const std::string& path, CrashMarker* out,
                                CrashBacktrace* bt = nullptr);

  // crash-loop 判定（安全模式场景的信号源）：窗口内崩溃次数 ≥ 阈值。
  static bool InCrashLoop(const std::string& path, uint32_t crashes_threshold,
                          int64_t window_ms, int64_t now_wall_ms);
};

}  // namespace imobs
