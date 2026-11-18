#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "madobs/observer.h"
namespace imobs {
// Additive queue, does not change the legacy 64-byte Event or disk formats.
class GenericEventQueue {
 public:
  static constexpr uint64_t kCapacity = 1024;
  GenericEventQueue() {
    for (uint64_t i = 0; i < kCapacity; ++i) slots_[i].seq.store(i);
  }
  GenericEventQueue(const GenericEventQueue&) = delete;
  bool Push(const madobs_event_v1& event) {
    uint64_t pos = tail_.load(std::memory_order_relaxed);
    for (unsigned budget = 0; budget < 64; ++budget) {
      auto& s = slots_[pos & (kCapacity - 1)];
      auto seq = s.seq.load(std::memory_order_acquire);
      auto diff = static_cast<int64_t>(seq - pos);
      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1,
                                        std::memory_order_relaxed)) {
          s.event = event;
          s.seq.store(pos + 1, std::memory_order_release);
          enqueued.fetch_add(1, std::memory_order_relaxed);
          return true;
        }
      } else if (diff < 0)
        break;
      else
        pos = tail_.load(std::memory_order_relaxed);
    }
    dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  bool Pop(madobs_event_v1& event) {
    auto& s = slots_[head_ & (kCapacity - 1)];
    if (s.seq.load(std::memory_order_acquire) != head_ + 1) return false;
    event = s.event;
    s.seq.store(head_ + kCapacity, std::memory_order_release);
    ++head_;
    return true;
  }
  std::atomic<uint64_t> enqueued{0}, dropped{0};

 private:
  struct alignas(128) Slot {
    std::atomic<uint64_t> seq{0};
    madobs_event_v1 event{};
  };
  std::array<Slot, kCapacity> slots_;
  alignas(128) std::atomic<uint64_t> tail_{0};
  alignas(128) uint64_t head_ = 0;
};
bool ValidGenericEvent(const madobs_event_v1* event);
void EncodeGenericEvent(const madobs_event_v1& event,
                        std::vector<uint8_t>* out);
bool DecodeGenericEvent(const uint8_t* data, size_t len, madobs_event_v1* out);
}  // namespace imobs
