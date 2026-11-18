// 热路径基准（实施计划 P0.2 决策门）：
//   门禁口径 = 4 生产者并发下 TryPush 的 P99 < 1000ns（含两次 steady_clock 计时开销）。
//   IMOBS_BENCH_GATE=1 时不达标退出非零（bootstrap/CI 用）；否则仅告警。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "imobs/event_ring.h"

using imobs::Event;
using imobs::EventRing;
using Clock = std::chrono::steady_clock;

namespace {

struct Percentiles {
  double p50, p90, p99, p999;
  uint32_t max;
};

Percentiles Summarize(std::vector<uint32_t>& ns) {
  std::sort(ns.begin(), ns.end());
  const auto at = [&](double q) {
    return static_cast<double>(ns[static_cast<size_t>(q * static_cast<double>(ns.size() - 1))]);
  };
  return {at(0.50), at(0.90), at(0.99), at(0.999), ns.back()};
}

struct CaseResult {
  Percentiles pct;
  uint64_t dropped = 0;
};

// pace_ns > 0 时每次打点后补齐间隔（配速不计入打点延迟）——覆盖"消费者跟得上"
// 的现实速率场景；pace_ns = 0 为饱和压测（此时部分打点会走满则丢弃的快路径，
// 丢弃量在结果里如实呈现，这正是"打点方永不阻塞"的语义本身）。
template <class Ring>
CaseResult RunCase(const char* tag, Ring& ring, unsigned producers, size_t ops_per_producer,
                   uint64_t pace_ns = 0) {
  std::atomic<bool> done{false};
  std::atomic<uint64_t> consumed{0};

  std::thread consumer([&] {
    Event buf[512];
    for (;;) {
      size_t n = ring.PopBatch(buf, 512);
      if (n == 0) {
        if (done.load(std::memory_order_acquire)) {
          if (ring.PopBatch(buf, 512) == 0) break;
          continue;
        }
        std::this_thread::yield();
        continue;
      }
      consumed.fetch_add(n, std::memory_order_relaxed);
    }
  });

  std::vector<std::vector<uint32_t>> samples(producers);
  std::vector<std::thread> threads;
  const auto wall0 = Clock::now();
  for (unsigned p = 0; p < producers; ++p) {
    threads.emplace_back([&samples, &ring, p, ops_per_producer, pace_ns] {
      auto& s = samples[p];
      s.resize(ops_per_producer);
      Event ev{};
      ev.attr_a = p;
      for (size_t k = 0; k < ops_per_producer; ++k) {
        ev.value = k;
        const auto t0 = Clock::now();
        ring.TryPush(ev);
        const auto t1 = Clock::now();
        s[k] = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (pace_ns != 0) {
          const auto next = t1 + std::chrono::nanoseconds(pace_ns);
          while (Clock::now() < next) {
          }
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  const auto wall1 = Clock::now();
  done.store(true, std::memory_order_release);
  consumer.join();

  std::vector<uint32_t> all;
  all.reserve(producers * ops_per_producer);
  for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
  const Percentiles pct = Summarize(all);
  const double wall_s = std::chrono::duration<double>(wall1 - wall0).count();
  const double mops = static_cast<double>(producers * ops_per_producer) / wall_s / 1e6;
  std::printf(
      "  %-8s producers=%u ops=%zu | push p50=%.0f p90=%.0f p99=%.0f p999=%.0f max=%u (ns)"
      " | 吞吐=%.1f Mops/s | dropped=%llu consumed=%llu\n",
      tag, producers, producers * ops_per_producer, pct.p50, pct.p90, pct.p99, pct.p999, pct.max,
      mops, static_cast<unsigned long long>(ring.dropped()),
      static_cast<unsigned long long>(consumed.load()));
  return CaseResult{pct, ring.dropped()};
}

}  // namespace

int main() {
  std::printf("EventRing 热路径基准（门禁：4 分片 × 4 生产者 P99 < 1000ns，含计时开销）\n");
  std::printf("-- 单环（对照组：全部生产者打同一 tail）\n");
  {
    EventRing r(1 << 14);
    RunCase("单环", r, 1, 1'000'000);
  }
  {
    EventRing r(1 << 14);
    RunCase("单环", r, 4, 500'000);
  }
  std::printf("-- 分片环（生产配置：线程轮转绑定分片，总容量同为 16K 槽）\n");
  {
    imobs::ShardedEventRing r(4, 1 << 12);
    RunCase("4分片", r, 1, 1'000'000);
  }
  CaseResult sat{};
  {
    imobs::ShardedEventRing r(4, 1 << 12);
    sat = RunCase("4分片", r, 4, 500'000);
  }
  {
    imobs::ShardedEventRing r(8, 1 << 11);
    RunCase("8分片", r, 8, 250'000);
  }
  std::printf("-- 现实速率（每生产者 1μs 间隔 ≈ 聚合 4M ops/s，消费者可持续跟上）\n");
  CaseResult paced{};
  {
    imobs::ShardedEventRing r(4, 1 << 12);
    paced = RunCase("4分片·配速", r, 4, 200'000, /*pace_ns=*/1000);
  }

  // 双门禁：① 饱和压测打点方 P99 < 1μs（含丢弃快路径——"永不阻塞"的语义本身）；
  //         ② 现实速率 P99 < 1μs 且零丢弃（消费者可持续、成功路径同样达标）。
  const bool gate_on = std::getenv("IMOBS_BENCH_GATE") != nullptr;
  bool pass = true;
  if (sat.pct.p99 >= 1000.0) {
    std::printf("[GATE-1] 饱和 4 分片 × 4 生产者 P99 = %.0fns >= 1000ns —— 不达标\n", sat.pct.p99);
    pass = false;
  } else {
    std::printf("[GATE-1] 饱和 P99 = %.0fns < 1000ns —— 通过（丢弃 %llu 条已记账）\n", sat.pct.p99,
                static_cast<unsigned long long>(sat.dropped));
  }
  if (paced.pct.p99 >= 1000.0 || paced.dropped != 0) {
    std::printf("[GATE-2] 配速 P99 = %.0fns / 丢弃 %llu —— 不达标\n", paced.pct.p99,
                static_cast<unsigned long long>(paced.dropped));
    pass = false;
  } else {
    std::printf("[GATE-2] 配速 P99 = %.0fns < 1000ns 且零丢弃 —— 通过\n", paced.pct.p99);
  }
  if (!pass && gate_on) return 1;
  return 0;
}
