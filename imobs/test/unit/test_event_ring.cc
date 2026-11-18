#include "imobs/event_ring.h"

#include <atomic>
#include <thread>
#include <vector>

#include "imobs_test.h"

using imobs::Event;
using imobs::EventRing;

namespace {
Event MakeEv(uint32_t producer, uint64_t seq) {
  Event ev{};
  ev.kind = static_cast<uint8_t>(imobs::EventKind::kMetric);
  ev.attr_a = producer;
  ev.value = seq;
  return ev;
}
}  // namespace

TEST(EventRing, FillDropWrap) {
  EventRing ring(8);
  for (uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(ring.TryPush(MakeEv(0, i)));
  EXPECT_FALSE(ring.TryPush(MakeEv(0, 8)));  // 满：丢弃并记账，绝不阻塞
  EXPECT_EQ(ring.dropped(), 1u);

  Event buf[8];
  ASSERT_EQ(ring.PopBatch(buf, 8), size_t{8});
  for (uint64_t i = 0; i < 8; ++i) EXPECT_EQ(buf[i].value, i);

  // 槽位回收后环回继续写
  EXPECT_TRUE(ring.TryPush(MakeEv(0, 9)));
  ASSERT_EQ(ring.PopBatch(buf, 8), size_t{1});
  EXPECT_EQ(buf[0].value, 9u);
}

TEST(EventRing, DropNeverBlocksWithoutConsumer) {
  EventRing ring(1024);
  uint64_t pushed = 0;
  for (int i = 0; i < 10 * 1024; ++i) {
    if (ring.TryPush(MakeEv(0, static_cast<uint64_t>(i)))) ++pushed;
  }
  EXPECT_EQ(pushed, 1024u);
  EXPECT_EQ(ring.dropped(), 10u * 1024 - 1024);
}

TEST(EventRing, MultiProducerOrderAndConservation) {
  constexpr int kProducers = 4;
  constexpr uint64_t kOps = 50'000;
  EventRing ring(1 << 12);

  std::atomic<bool> done{false};
  std::atomic<uint64_t> pushed_ok{0};
  uint64_t popped = 0;
  uint64_t last_seen[kProducers] = {0, 0, 0, 0};
  bool order_ok = true;

  std::thread consumer([&] {
    Event buf[256];
    for (;;) {
      size_t n = ring.PopBatch(buf, 256);
      if (n == 0) {
        if (done.load(std::memory_order_acquire)) {
          n = ring.PopBatch(buf, 256);
          if (n == 0) break;
        } else {
          std::this_thread::yield();
          continue;
        }
      }
      for (size_t i = 0; i < n; ++i) {
        const uint32_t p = buf[i].attr_a;
        if (!(buf[i].value > last_seen[p])) order_ok = false;  // 每生产者 FIFO
        last_seen[p] = buf[i].value;
      }
      popped += n;
    }
  });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&ring, &pushed_ok, p] {
      for (uint64_t k = 1; k <= kOps; ++k) {
        if (ring.TryPush(MakeEv(static_cast<uint32_t>(p), k))) {
          pushed_ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& t : producers) t.join();
  done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_TRUE(order_ok);
  EXPECT_EQ(popped, pushed_ok.load());
  EXPECT_EQ(popped + ring.dropped(), kProducers * kOps);  // 守恒：进 = 出 + 丢（有账）
}

TEST(ShardedRing, ConservationAndPerProducerFifo) {
  constexpr int kProducers = 4;
  constexpr uint64_t kOps = 50'000;
  imobs::ShardedEventRing ring(4, 1 << 10);

  std::atomic<bool> done{false};
  std::atomic<uint64_t> pushed_ok{0};
  uint64_t popped = 0;
  uint64_t last_seen[kProducers] = {0, 0, 0, 0};
  bool order_ok = true;

  std::thread consumer([&] {
    Event buf[256];
    for (;;) {
      size_t n = ring.PopBatch(buf, 256);
      if (n == 0) {
        if (done.load(std::memory_order_acquire)) {
          n = ring.PopBatch(buf, 256);
          if (n == 0) break;
        } else {
          std::this_thread::yield();
          continue;
        }
      }
      for (size_t i = 0; i < n; ++i) {
        const uint32_t p = buf[i].attr_a;
        // 线程终生绑定分片 → 每生产者 FIFO 跨分片合并后依然成立
        if (!(buf[i].value > last_seen[p])) order_ok = false;
        last_seen[p] = buf[i].value;
      }
      popped += n;
    }
  });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&ring, &pushed_ok, p] {
      for (uint64_t k = 1; k <= kOps; ++k) {
        if (ring.TryPush(MakeEv(static_cast<uint32_t>(p), k))) {
          pushed_ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& t : producers) t.join();
  done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_TRUE(order_ok);
  EXPECT_EQ(popped, pushed_ok.load());
  EXPECT_EQ(popped + ring.dropped(), kProducers * kOps);
}

IMOBS_TEST_MAIN
