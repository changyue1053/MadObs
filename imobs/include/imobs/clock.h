#pragma once
// 时间铁律（架构 §8）：端内一律单调钟，墙钟只出现在批级锚点对里——
// 墙钟跳变（用户改时间 / NTP）会伪造时延指标，锚点换算把它挡在体系之外。

#include <atomic>
#include <chrono>
#include <cstdint>

namespace imobs {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint64_t MonoNanos() = 0;
  virtual int64_t WallMillis() = 0;
};

class RealClock final : public IClock {
 public:
  uint64_t MonoNanos() override {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  int64_t WallMillis() override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  static RealClock& Instance() {
    static RealClock c;
    return c;
  }
};

// 虚拟时钟：确定性测试的地基——防抖窗口、冷却期、退避全部可快进（实施计划 P0.4）。
class VirtualClock final : public IClock {
 public:
  explicit VirtualClock(uint64_t mono_ns = 0, int64_t wall_ms = 1'700'000'000'000)
      : mono_(mono_ns), wall_(wall_ms) {}
  uint64_t MonoNanos() override { return mono_.load(std::memory_order_relaxed); }
  int64_t WallMillis() override { return wall_.load(std::memory_order_relaxed); }
  void AdvanceNanos(uint64_t d) {
    mono_.fetch_add(d, std::memory_order_relaxed);
    wall_.fetch_add(static_cast<int64_t>(d / 1'000'000), std::memory_order_relaxed);
  }
  void AdvanceMillis(uint64_t ms) { AdvanceNanos(ms * 1'000'000ull); }

 private:
  std::atomic<uint64_t> mono_;
  std::atomic<int64_t> wall_;
};

// 批级锚点对：端侧事件只带单调钟，每个上报批附一对 (wall, mono)，
// 服务端拼接时以此把端侧偏移换算到统一时间轴（架构 §6 时钟锚定）。
struct ClockAnchor {
  int64_t wall_ms = 0;
  uint64_t mono_ns = 0;
};

inline int64_t WallFromMono(const ClockAnchor& a, uint64_t mono_ns) {
  const int64_t delta_ms =
      (static_cast<int64_t>(mono_ns) - static_cast<int64_t>(a.mono_ns)) / 1'000'000;
  return a.wall_ms + delta_ms;
}

}  // namespace imobs
