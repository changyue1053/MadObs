#ifndef MADOBS_PRIVATE_H
#define MADOBS_PRIVATE_H
#include "madobs/runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Owning native C binding for managed adapters. Handles belong to this API
 * only. Host callback/context must live through stop/destroy; neither may be
 * called from an exporter callback. No raw handle or unfiltered emit is exposed
 * here. */
/* Legacy Open returns 0; OpenEx additionally reports a reason. A single lifecycle
 * owner must coordinate destruction: INVALID_HANDLE means revoked/unknown, NOT
 * that another destroy has completed draining. Stop waits while handle remains
 * valid, then releases the WAL lock. Retaining a stopped handle for stats does
 * not reserve the directory against another opener. */
typedef enum {
  MADOBS_PRIVATE_OPEN_OK = 0,
  MADOBS_PRIVATE_OPEN_INVALID_ARGUMENT = 1,
  MADOBS_PRIVATE_OPEN_INVALID_POLICY = 2,
  MADOBS_PRIVATE_OPEN_WAL_UNAVAILABLE = 3,
  MADOBS_PRIVATE_OPEN_WAL_UNSAFE = 4,
  MADOBS_PRIVATE_OPEN_WAL_UNBOUND = 5,
  MADOBS_PRIVATE_OPEN_POLICY_OR_MANIFEST_MISMATCH = 6,
  MADOBS_PRIVATE_OPEN_WAL_BUSY = 7,
  MADOBS_PRIVATE_OPEN_WAL_IO_ERROR = 8,
  MADOBS_PRIVATE_OPEN_NATIVE_CREATE_FAILED = 9,
  MADOBS_PRIVATE_OPEN_NATIVE_START_FAILED = 10,
  MADOBS_PRIVATE_OPEN_BINDING_CAPACITY = 11,
  MADOBS_PRIVATE_OPEN_INTERNAL = 12
} madobs_private_open_result;
typedef uint64_t madobs_private_handle_t;
typedef struct {
  uint64_t rejected_attempts;
  uint64_t rejected_by_priority[3];
  uint64_t last_batch_id;
  uint32_t last_priority;
  uint32_t last_reason;
} madobs_private_gate_stats_v1;
IMOBS_API madobs_private_handle_t
madobs_private_open(const char* wal_dir, madobs_channel_send_fn send,
                    void* context, const madobs_privacy_policy_v1* policy);
/* Optional diagnostic output contains a reason code only, never path/payload.
 * Mismatch does not distinguish corruption from a different policy. */
IMOBS_API madobs_private_handle_t madobs_private_open_ex(
    const char* wal_dir, madobs_channel_send_fn send, void* context,
    const madobs_privacy_policy_v1* policy, madobs_private_open_result* result);
IMOBS_API madobs_status_t madobs_private_emit(madobs_private_handle_t handle,
                                              const madobs_event_v1* event);
IMOBS_API madobs_status_t madobs_private_stop(madobs_private_handle_t handle);
IMOBS_API madobs_status_t
madobs_private_destroy(madobs_private_handle_t handle);
IMOBS_API madobs_status_t
madobs_private_get_stats(madobs_private_handle_t handle,
                         madobs_generic_stats_v1* out, size_t out_size);
IMOBS_API madobs_status_t madobs_private_get_gate_stats(
    madobs_private_handle_t handle, madobs_private_gate_stats_v1* out,
    size_t out_size);
#ifdef __cplusplus
}
#endif
#endif
