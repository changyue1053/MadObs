#pragma once
// 热路径审计区（scripts/audit_hotpath.py）：本文件禁止锁 / 堆分配 / 系统调用。
// 构造函数是唯一例外（冷路径，进程启动时一次），经由 make_unique 分配槽数组。

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "imobs/event.h"

namespace imobs {

namespace detail {
// spin 减压指令：降低 CAS 竞争时的缓存一致性风暴（Apple Silicon 无 SMT，isb 是
// 业界通行的 pause 等价物，WebKit/folly 同款做法）。
inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}
}  // namespace detail

// Apple Silicon 缓存行 128B（x86 为 64B，按大取齐）：槽位与热点计数器按缓存行
// 隔离，消除"生产者写槽 i / 消费者读槽 i±1"的伪共享——P0.2 门禁调优的关键项。
inline constexpr size_t kCacheLine = 128;

// 有界 MPMC 环形队列——Dmitry Vyukov 有界队列的"丢弃变体"（实施计划 P0.2）：
//   - 任意线程 TryPush：无锁（有界 CAS 竞争 + 有界退避），队列满时丢弃并记账，
//     绝不阻塞打点方（压舱石第二条：反压传导到打点方 = 观测伤害主链路）；
//   - 唯一消费线程 PopBatch：无 CAS——head 是消费线程私有状态；
//   - 正确性：数据可见性由 slot.seq 的 release/acquire 配对保证，
//     tail 的 CAS 只做序号分配，relaxed 足够（与 Vyukov 原始实现一致）。
// 内存账：每槽 128B（64B 事件 + seq + 行对齐），默认 16K 槽 = 2MB，在 8MB 预算内。
class EventRing {
 public:
  explicit EventRing(size_t capacity_pow2)
      : capacity_(capacity_pow2),
        mask_(capacity_pow2 - 1),
        slots_(std::make_unique<Slot[]>(capacity_pow2)) {  // audit-allow: ctor-once 构造期一次分配槽数组（文件头注明的唯一例外）
    assert(capacity_pow2 >= 2 && (capacity_pow2 & (capacity_pow2 - 1)) == 0);
    for (size_t i = 0; i < capacity_; ++i) {
      slots_[i].seq.store(i, std::memory_order_relaxed);
    }
  }
  EventRing(const EventRing&) = delete;
  EventRing& operator=(const EventRing&) = delete;

  // 任意线程调用。返回 false = 队列满已丢弃（丢弃量本身是 SLI）。
  bool TryPush(const Event& ev) {
    uint64_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots_[pos & mask_];
      const uint64_t seq = s.seq.load(std::memory_order_acquire);
      const int64_t dif = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          s.data = ev;
          s.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
        detail::CpuRelax();  // CAS 失败 = 真竞争：让一拍缓存行（指数退避会牺牲尾延迟）
      } else if (dif < 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  // 仅允许唯一消费线程调用；返回本批取出的事件数。
  size_t PopBatch(Event* out, size_t max) {
    size_t n = 0;
    while (n < max) {
      Slot& s = slots_[head_ & mask_];
      const uint64_t seq = s.seq.load(std::memory_order_acquire);
      if (static_cast<int64_t>(seq) - static_cast<int64_t>(head_ + 1) != 0) break;
      out[n++] = s.data;
      s.seq.store(head_ + capacity_, std::memory_order_release);
      ++head_;
    }
    return n;
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  size_t capacity() const { return capacity_; }

 private:
  struct alignas(kCacheLine) Slot {  // 一槽一缓存行：生产/消费互不打扰
    std::atomic<uint64_t> seq{0};
    Event data{};
  };
  static_assert(sizeof(Slot) == kCacheLine);

  const size_t capacity_;
  const size_t mask_;
  std::unique_ptr<Slot[]> slots_;

  alignas(kCacheLine) std::atomic<uint64_t> tail_{0};  // 生产者共享（独占缓存行）
  alignas(kCacheLine) uint64_t head_{0};               // 消费线程私有
  alignas(kCacheLine) std::atomic<uint64_t> dropped_{0};
};

// 分片环（P0.2 门禁调优的定稿方案，对应实施计划 R1 预案）：
// 打点方线程经 thread-local 轮转绑定到固定分片——绑定后每分片趋近单生产者，
// tail CAS 竞争按分片数摊薄；消费线程轮转排空全部分片。
// 语义保持：每生产者 FIFO（线程终生绑定同一分片）、满则丢弃有账、单消费线程。
class ShardedEventRing {
 public:
  ShardedEventRing(size_t shard_count_pow2, size_t capacity_per_shard_pow2)
      : smask_(shard_count_pow2 - 1) {
    assert(shard_count_pow2 >= 1 && (shard_count_pow2 & (shard_count_pow2 - 1)) == 0);
    shards_.reserve(shard_count_pow2);  // audit-allow: ctor-once 冷路径容器，仅构造期扩容
    for (size_t i = 0; i < shard_count_pow2; ++i) {
      shards_.push_back(std::make_unique<EventRing>(capacity_per_shard_pow2));  // audit-allow: ctor-once 构造期逐分片一次分配
    }
  }

  bool TryPush(const Event& ev) { return shards_[ShardOfThisThread() & smask_]->TryPush(ev); }

  // 仅唯一消费线程调用：轮转起点保证各分片公平排空。
  size_t PopBatch(Event* out, size_t max) {
    size_t n = 0;
    const size_t sc = smask_ + 1;
    for (size_t i = 0; i < sc && n < max; ++i) {
      n += shards_[(rr_ + i) & smask_]->PopBatch(out + n, max - n);
    }
    rr_ = (rr_ + 1) & smask_;
    return n;
  }

  uint64_t dropped() const {
    uint64_t d = 0;
    for (const auto& s : shards_) d += s->dropped();
    return d;
  }
  size_t capacity() const { return (smask_ + 1) * shards_[0]->capacity(); }

 private:
  // 线程 → 分片：进程级计数器轮转分配，thread-local 缓存（arm64 上 TLS 读约 1ns）。
  static uint32_t ShardOfThisThread() {
    static std::atomic<uint32_t> next{0};
    thread_local uint32_t shard = next.fetch_add(1, std::memory_order_relaxed);
    return shard;
  }

  const size_t smask_;
  size_t rr_ = 0;  // 消费线程私有
  std::vector<std::unique_ptr<EventRing>> shards_;  // audit-allow: ctor-once 构造后只读
};

}  // namespace imobs
