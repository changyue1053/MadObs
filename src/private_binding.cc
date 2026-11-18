#include <map>
#include <mutex>

#include "madobs/private.h"
#include "madobs/private_runtime.hpp"
namespace {
struct BindingRegistry {
  std::mutex mutex;
  std::map<madobs_private_handle_t, std::shared_ptr<madobs::PrivateRuntime>>
      entries;
  uint64_t generation = 0;
};
BindingRegistry& Registry() {
  static BindingRegistry registry;
  return registry;
}
std::shared_ptr<madobs::PrivateRuntime> Acquire(
    madobs_private_handle_t handle) {
  std::lock_guard<std::mutex> lock(Registry().mutex);
  auto it = Registry().entries.find(handle);
  return it == Registry().entries.end() ? nullptr : it->second;
}
}  // namespace
madobs_private_handle_t madobs_private_open_ex(
    const char* dir, madobs_channel_send_fn send, void* context,
    const madobs_privacy_policy_v1* policy,
    madobs_private_open_result* diagnostic) {
  madobs_private_open_result ignored = MADOBS_PRIVATE_OPEN_INTERNAL;
  if (!diagnostic) diagnostic = &ignored;
  *diagnostic = MADOBS_PRIVATE_OPEN_INTERNAL;
  try {
    auto runtime =
        madobs::PrivateRuntime::Open(dir, send, context, policy, diagnostic);
    if (!runtime) return 0;
    std::lock_guard<std::mutex> lock(Registry().mutex);
    if (Registry().entries.size() >= 64 ||
        Registry().generation == 0x0000ffffffffffffULL) {
      *diagnostic = MADOBS_PRIVATE_OPEN_BINDING_CAPACITY;
      return 0;
    }
    const auto handle = 0x4d4f000000000000ULL | ++Registry().generation;
    Registry().entries.emplace(handle, std::move(runtime));
    *diagnostic = MADOBS_PRIVATE_OPEN_OK;
    return handle;
  } catch (...) {
    *diagnostic = MADOBS_PRIVATE_OPEN_INTERNAL;
    return 0;
  }
}
madobs_private_handle_t madobs_private_open(
    const char* dir, madobs_channel_send_fn send, void* context,
    const madobs_privacy_policy_v1* policy) {
  return madobs_private_open_ex(dir, send, context, policy, nullptr);
}
madobs_status_t madobs_private_emit(madobs_private_handle_t h,
                                    const madobs_event_v1* event) {
  if (!event) return MADOBS_E_INVALID_ARG;
  try {
    auto runtime = Acquire(h);
    return runtime ? runtime->Emit(*event) : MADOBS_E_INVALID_HANDLE;
  } catch (...) {
    return MADOBS_E_INTERNAL;
  }
}
madobs_status_t madobs_private_stop(madobs_private_handle_t h) {
  try {
    auto runtime = Acquire(h);
    return runtime ? runtime->Close() : MADOBS_E_INVALID_HANDLE;
  } catch (...) {
    return MADOBS_E_INTERNAL;
  }
}
madobs_status_t madobs_private_destroy(madobs_private_handle_t h) {
  try {
    std::shared_ptr<madobs::PrivateRuntime> runtime;
    {
      std::lock_guard<std::mutex> lock(Registry().mutex);
      auto it = Registry().entries.find(h);
      if (it == Registry().entries.end()) return MADOBS_E_INVALID_HANDLE;
      runtime = std::move(it->second);
      Registry().entries.erase(it);
    }
    return runtime->Close();
  } catch (...) {
    return MADOBS_E_INTERNAL;
  }
}
madobs_status_t madobs_private_get_stats(madobs_private_handle_t h,
                                         madobs_generic_stats_v1* out,
                                         size_t n) {
  if (!out || n < sizeof(*out)) return MADOBS_E_INVALID_ARG;
  try {
    auto runtime = Acquire(h);
    return runtime ? runtime->Stats(*out) : MADOBS_E_INVALID_HANDLE;
  } catch (...) {
    return MADOBS_E_INTERNAL;
  }
}
madobs_status_t madobs_private_get_gate_stats(madobs_private_handle_t h,
                                              madobs_private_gate_stats_v1* out,
                                              size_t n) {
  if (!out || n < sizeof(*out)) return MADOBS_E_INVALID_ARG;
  try {
    auto runtime = Acquire(h);
    if (!runtime) return MADOBS_E_INVALID_HANDLE;
    *out = runtime->GateStats();
    return MADOBS_OK;
  } catch (...) {
    return MADOBS_E_INTERNAL;
  }
}
