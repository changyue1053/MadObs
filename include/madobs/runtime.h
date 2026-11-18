#ifndef MADOBS_RUNTIME_H
#define MADOBS_RUNTIME_H
#include "imobs/imobs_c.h" /* bundled compatibility types, not another SDK */
#include "madobs/observer.h"
#include "madobs/privacy.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  MADOBS_PRIVATE_BATCH_OK = 0,
  MADOBS_PRIVATE_BAD_POLICY = 1,
  MADOBS_PRIVATE_BAD_WIRE = 2,
  MADOBS_PRIVATE_UNSAFE_RECORD = 3,
  MADOBS_PRIVATE_NONCANONICAL = 4,
  MADOBS_PRIVATE_RESOURCE_FAILURE = 5
} madobs_private_batch_result;
IMOBS_API madobs_private_batch_result madobs_check_private_batch(
    const uint8_t* data, size_t len, const madobs_privacy_policy_v1* policy);
/* Reject the entire batch unless every record is a canonical policy-safe G1
 * event. Does not remove batch timestamps or guarantee transport privacy. */
IMOBS_API int madobs_validate_private_batch(
    const uint8_t* data, size_t len, const madobs_privacy_policy_v1* policy);
typedef imobs_action_spec_t madobs_action_spec_t;
typedef imobs_channel_send_fn madobs_channel_send_fn;
typedef imobs_handle_t madobs_handle_t;
typedef imobs_heal_done_fn madobs_heal_done_fn;
typedef imobs_heal_execute_fn madobs_heal_execute_fn;
typedef imobs_heal_verify_fn madobs_heal_verify_fn;
typedef imobs_label_def_t madobs_label_def_t;
typedef imobs_scene_spec_t madobs_scene_spec_t;
typedef imobs_self_stats_t madobs_self_stats_t;
typedef imobs_send_done_fn madobs_send_done_fn;
typedef imobs_status_t madobs_status_t;
typedef imobs_tuning_t madobs_tuning_t;
#define MADOBS_E_BAD_STATE IMOBS_E_BAD_STATE
#define MADOBS_E_DISABLED IMOBS_E_DISABLED
#define MADOBS_E_INTERNAL IMOBS_E_INTERNAL
#define MADOBS_E_INVALID_ARG IMOBS_E_INVALID_ARG
#define MADOBS_E_INVALID_HANDLE IMOBS_E_INVALID_HANDLE
#define MADOBS_E_OPEN_FAILED IMOBS_E_OPEN_FAILED
#define MADOBS_E_SCHEMA_REJECTED IMOBS_E_SCHEMA_REJECTED
#define MADOBS_E_TOO_MANY_INSTANCES IMOBS_E_TOO_MANY_INSTANCES
#define MADOBS_OK IMOBS_OK
IMOBS_API madobs_handle_t madobs_create(const char* wal_dir,
                                        madobs_channel_send_fn send,
                                        void* channel_ctx);
IMOBS_API madobs_status_t madobs_set_control_key(madobs_handle_t h,
                                                 const uint8_t* key,
                                                 size_t len);
IMOBS_API madobs_status_t madobs_set_tuning(madobs_handle_t h,
                                            const madobs_tuning_t* tuning);
IMOBS_API madobs_status_t madobs_register_counter(
    madobs_handle_t h, const char* name, const madobs_label_def_t* labels,
    size_t n_labels, uint64_t* out_key);
IMOBS_API madobs_status_t madobs_register_histogram(
    madobs_handle_t h, const char* name, const madobs_label_def_t* labels,
    size_t n_labels, uint64_t* out_key);
IMOBS_API madobs_status_t madobs_resolve_labels(madobs_handle_t h, uint64_t key,
                                                const char* const* keys,
                                                const char* const* values,
                                                size_t n, uint16_t* out_idx);
IMOBS_API madobs_status_t
madobs_register_heal_scene(madobs_handle_t h, const madobs_scene_spec_t* spec,
                           madobs_heal_execute_fn execute,
                           madobs_heal_verify_fn verify, void* exec_ctx);
IMOBS_API madobs_status_t madobs_enable_logs(madobs_handle_t h,
                                             const char* path,
                                             uint32_t segment_size,
                                             uint32_t segment_count,
                                             const uint8_t* key32);
IMOBS_API madobs_status_t madobs_start(madobs_handle_t h);
IMOBS_API madobs_status_t madobs_stop(madobs_handle_t h);
IMOBS_API madobs_status_t madobs_destroy(madobs_handle_t h);
IMOBS_API void madobs_counter_add(madobs_handle_t h, uint64_t key,
                                  uint16_t label_idx, uint64_t delta);
IMOBS_API void madobs_histogram_record(madobs_handle_t h, uint64_t key,
                                       uint16_t label_idx, uint64_t value);
IMOBS_API void madobs_trace_stage(madobs_handle_t h, uint64_t msg_id,
                                  uint8_t stage, uint8_t status, uint32_t attr,
                                  uint64_t value);
IMOBS_API void madobs_emit_event(madobs_handle_t h, uint64_t key, uint8_t level,
                                 uint32_t a, uint32_t b);
IMOBS_API void madobs_health_signal(madobs_handle_t h, uint64_t signal_id,
                                    int active);
IMOBS_API void madobs_global_outage(madobs_handle_t h, int outage);
IMOBS_API void madobs_set_foreground(madobs_handle_t h, int foreground);
IMOBS_API void madobs_submit_control(madobs_handle_t h, const uint8_t* data,
                                     size_t len);
IMOBS_API void madobs_log(madobs_handle_t h, uint8_t level,
                          uint64_t module_hash, const char* text,
                          size_t text_len);
IMOBS_API void madobs_set_fetch_consent(madobs_handle_t h, int granted);
IMOBS_API madobs_status_t madobs_crash_install(const char* marker_path);
IMOBS_API int32_t madobs_crash_consume_pending(madobs_handle_t h,
                                               const char* marker_path);
IMOBS_API int32_t madobs_crash_in_loop(const char* marker_path,
                                       uint32_t threshold, int64_t window_ms,
                                       int64_t now_wall_ms);
IMOBS_API uint64_t madobs_name_hash(const char* name);
IMOBS_API madobs_status_t madobs_get_self_stats(madobs_handle_t h,
                                                madobs_self_stats_t* out);

#define MADOBS_E_QUEUE_FULL ((madobs_status_t)9)
typedef struct {
  uint64_t runtime_handle;
} madobs_observer_binding_v1;
typedef struct {
  uint64_t enqueued, drained, queue_dropped, wal_dropped;
} madobs_generic_stats_v1;
/* Shutdown order: stop/join every producer, then stop runtime, then destroy it.
 * emit concurrent with stop/destroy is NOT supported. stop is terminal; start
 * after stop returns BAD_STATE. Create a new instance to restart.
 * Binding storage belongs to host and must outlive all producers. No concurrent
 * rebinding/free or runtime destroy. A stale sequential runtime handle is safe.
 */
IMOBS_API madobs_status_t madobs_observer_for_runtime(
    madobs_handle_t h, madobs_observer_binding_v1* binding,
    madobs_observer_v1* out);
IMOBS_API madobs_status_t
madobs_emit_generic_event(madobs_handle_t h, const madobs_event_v1* event);
IMOBS_API madobs_status_t
madobs_get_generic_stats(madobs_handle_t h, madobs_generic_stats_v1* out);
/* Decode a single G1 record after batch framing/decompression. out_size avoids
 * writing beyond older caller buffers. Failure leaves out unchanged. */
IMOBS_API madobs_status_t madobs_decode_event_record(const uint8_t* data,
                                                     size_t len,
                                                     madobs_event_v1* out,
                                                     size_t out_size);
#ifdef __cplusplus
}
#endif
#endif
