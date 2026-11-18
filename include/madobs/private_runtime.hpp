#ifndef MADOBS_PRIVATE_RUNTIME_HPP
#define MADOBS_PRIVATE_RUNTIME_HPP
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "madobs/privacy.h"
#include "madobs/private.h"
#include "madobs/private_wal.hpp"
#include "madobs/runtime.h"
namespace madobs {
// Native ownership substrate for platform bindings. Not a process sandbox: raw
// legacy symbols remain available elsewhere. No raw handle/observer is exposed.
// Callers racing Close/Emit must each retain a shared_ptr; racing destruction
// of the same shared_ptr variable is not safe. Exporter callbacks must not
// block. Never Close or release the final owner from an exporter callback:
// worker thread shutdown must be performed by a separate host lifecycle thread.
class PrivateRuntime final {
 public:
  static std::shared_ptr<PrivateRuntime> Open(
      const char* wal_dir, madobs_channel_send_fn send, void* context,
      const madobs_privacy_policy_v1* policy,
      madobs_private_open_result* diagnostic = nullptr) {
    madobs_private_open_result ignored = MADOBS_PRIVATE_OPEN_INTERNAL;
    if (!diagnostic) diagnostic = &ignored;
    *diagnostic = MADOBS_PRIVATE_OPEN_INTERNAL;
    if (!wal_dir || !wal_dir[0] || !send) {
      *diagnostic = MADOBS_PRIVATE_OPEN_INVALID_ARGUMENT;
      return {};
    }
    if (!madobs_privacy_policy_valid(policy)) {
      *diagnostic = MADOBS_PRIVATE_OPEN_INVALID_POLICY;
      return {};
    }
    auto result = std::shared_ptr<PrivateRuntime>(new PrivateRuntime);
    result->policy_.event_count = policy->event_count;
    for (uint32_t i = 0; i < policy->event_count; ++i)
      result->rules_[i] = policy->events[i];
    if (!result->wal_lock_.Acquire(wal_dir, result->policy_, diagnostic))
      return {};
    result->send_ = send;
    result->context_ = context;
    result->handle_ =
        madobs_create(wal_dir, &PrivateRuntime::Send, result.get());
    if (!result->handle_) {
      *diagnostic = MADOBS_PRIVATE_OPEN_NATIVE_CREATE_FAILED;
      return {};
    }
    if (madobs_start(result->handle_) != MADOBS_OK) {
      *diagnostic = MADOBS_PRIVATE_OPEN_NATIVE_START_FAILED;
      return {};
    }
    *diagnostic = MADOBS_PRIVATE_OPEN_OK;
    return result;
  }
  PrivateRuntime(const PrivateRuntime&) = delete;
  PrivateRuntime& operator=(const PrivateRuntime&) = delete;
  PrivateRuntime(PrivateRuntime&&) = delete;
  PrivateRuntime& operator=(PrivateRuntime&&) = delete;
  ~PrivateRuntime() { Close(); }
  madobs_status_t Emit(const madobs_event_v1& event) {
    madobs_event_v1 clean{};
    if (madobs_sanitize_event(&policy_, &event, &clean) != MADOBS_EMITTED)
      return MADOBS_E_SCHEMA_REJECTED;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handle_) return MADOBS_E_BAD_STATE;
    return madobs_emit_generic_event(handle_, &clean);
  }
  // All concurrent Close callers wait for the same drain/destroy. No lock is
  // held during exporter callbacks, so callback-side Emit returns BAD_STATE.
  // Close flushes locally; it is not a remote delivery acknowledgement.
  madobs_status_t Close() {
    madobs_handle_t handle;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      closed_.wait(lock, [this] { return !closing_; });
      handle = handle_;
      if (!handle) return close_status_;
      closing_ = true;
      handle_ = 0;
    }
    const auto stop = madobs_stop(handle);
    madobs_generic_stats_v1 stats{};
    const auto snapshot = madobs_get_generic_stats(handle, &stats);
    const auto destroyed = madobs_destroy(handle);
    wal_lock_.Release();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      final_stats_ = stats;
      stats_status_ = snapshot;
      close_status_ = stop != MADOBS_OK ? stop : destroyed;
      closing_ = false;
    }
    closed_.notify_all();
    return close_status_;
  }
  madobs_status_t Stats(madobs_generic_stats_v1& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return MADOBS_E_BAD_STATE;
    if (handle_) return madobs_get_generic_stats(handle_, &out);
    if (stats_status_ == MADOBS_OK) out = final_stats_;
    return stats_status_;
  }

  uint64_t RejectedBatches() const { return rejected_batches_.load(); }
  madobs_private_gate_stats_v1 GateStats() const {
    std::lock_guard<std::mutex> lock(gate_mutex_);
    return gate_stats_;
  }

 private:
  static void Send(void* ctx, uint8_t priority, uint64_t id,
                   const uint8_t* data, size_t len, madobs_send_done_fn done,
                   void* done_ctx) {
    auto* self = static_cast<PrivateRuntime*>(ctx);
    const auto reason = madobs_check_private_batch(data, len, &self->policy_);
    if (reason != MADOBS_PRIVATE_BATCH_OK) {
      ++self->rejected_batches_;
      {
        std::lock_guard<std::mutex> lock(self->gate_mutex_);
        ++self->gate_stats_.rejected_attempts;
        if (priority < 3) ++self->gate_stats_.rejected_by_priority[priority];
        self->gate_stats_.last_batch_id = id;
        self->gate_stats_.last_priority = priority;
        self->gate_stats_.last_reason = static_cast<uint32_t>(reason);
      }
      done(done_ctx, 0);  // retain rejected WAL; never silently discard/ack it
      return;
    }
    self->send_(self->context_, priority, id, data, len, done, done_ctx);
  }
  PrivateRuntime()
      : policy_{sizeof(policy_), MADOBS_ABI_VERSION, 0, rules_.data()} {}
  std::array<madobs_event_rule_v1, MADOBS_SCHEMA_MAX_EVENTS> rules_{};
  madobs_privacy_policy_v1 policy_;
  detail::PrivateWalLock wal_lock_;
  madobs_channel_send_fn send_ = nullptr;
  void* context_ = nullptr;
  std::atomic<uint64_t> rejected_batches_{0};
  mutable std::mutex gate_mutex_;
  madobs_private_gate_stats_v1 gate_stats_{};
  std::mutex mutex_;
  madobs_handle_t handle_ = 0;
  std::condition_variable closed_;
  bool closing_ = false;
  madobs_status_t close_status_ = MADOBS_OK;
  madobs_status_t stats_status_ = MADOBS_E_BAD_STATE;
  madobs_generic_stats_v1 final_stats_{};
};
}  // namespace madobs
#endif
