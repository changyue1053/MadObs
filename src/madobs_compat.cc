#include <cstring>

#include "imobs/batch_view.h"
#include "imobs/codec.h"
#include "imobs/generic_events.h"
#include "imobs/wire.h"
#include "madobs/runtime.h"

madobs_handle_t madobs_create(const char* wal_dir, madobs_channel_send_fn send,
                              void* channel_ctx) {
  return imobs_create(wal_dir, send, channel_ctx);
}
madobs_status_t madobs_set_control_key(madobs_handle_t h, const uint8_t* key,
                                       size_t len) {
  return imobs_set_control_key(h, key, len);
}
madobs_status_t madobs_set_tuning(madobs_handle_t h,
                                  const madobs_tuning_t* tuning) {
  return imobs_set_tuning(h, tuning);
}
madobs_status_t madobs_register_counter(madobs_handle_t h, const char* name,
                                        const madobs_label_def_t* labels,
                                        size_t n_labels, uint64_t* out_key) {
  return imobs_register_counter(h, name, labels, n_labels, out_key);
}
madobs_status_t madobs_register_histogram(madobs_handle_t h, const char* name,
                                          const madobs_label_def_t* labels,
                                          size_t n_labels, uint64_t* out_key) {
  return imobs_register_histogram(h, name, labels, n_labels, out_key);
}
madobs_status_t madobs_resolve_labels(madobs_handle_t h, uint64_t key,
                                      const char* const* keys,
                                      const char* const* values, size_t n,
                                      uint16_t* out_idx) {
  return imobs_resolve_labels(h, key, keys, values, n, out_idx);
}
madobs_status_t madobs_register_heal_scene(madobs_handle_t h,
                                           const madobs_scene_spec_t* spec,
                                           madobs_heal_execute_fn execute,
                                           madobs_heal_verify_fn verify,
                                           void* exec_ctx) {
  return imobs_register_heal_scene(h, spec, execute, verify, exec_ctx);
}
madobs_status_t madobs_enable_logs(madobs_handle_t h, const char* path,
                                   uint32_t segment_size,
                                   uint32_t segment_count,
                                   const uint8_t* key32) {
  return imobs_enable_logs(h, path, segment_size, segment_count, key32);
}
madobs_status_t madobs_start(madobs_handle_t h) { return imobs_start(h); }
madobs_status_t madobs_stop(madobs_handle_t h) { return imobs_stop(h); }
madobs_status_t madobs_destroy(madobs_handle_t h) { return imobs_destroy(h); }
void madobs_counter_add(madobs_handle_t h, uint64_t key, uint16_t label_idx,
                        uint64_t delta) {
  imobs_counter_add(h, key, label_idx, delta);
}
void madobs_histogram_record(madobs_handle_t h, uint64_t key,
                             uint16_t label_idx, uint64_t value) {
  imobs_histogram_record(h, key, label_idx, value);
}
void madobs_trace_stage(madobs_handle_t h, uint64_t msg_id, uint8_t stage,
                        uint8_t status, uint32_t attr, uint64_t value) {
  imobs_trace_stage(h, msg_id, stage, status, attr, value);
}
void madobs_emit_event(madobs_handle_t h, uint64_t key, uint8_t level,
                       uint32_t a, uint32_t b) {
  imobs_emit_event(h, key, level, a, b);
}
void madobs_health_signal(madobs_handle_t h, uint64_t signal_id, int active) {
  imobs_health_signal(h, signal_id, active);
}
void madobs_global_outage(madobs_handle_t h, int outage) {
  imobs_global_outage(h, outage);
}
void madobs_set_foreground(madobs_handle_t h, int foreground) {
  imobs_set_foreground(h, foreground);
}
void madobs_submit_control(madobs_handle_t h, const uint8_t* data, size_t len) {
  imobs_submit_control(h, data, len);
}
void madobs_log(madobs_handle_t h, uint8_t level, uint64_t module_hash,
                const char* text, size_t text_len) {
  imobs_log(h, level, module_hash, text, text_len);
}
void madobs_set_fetch_consent(madobs_handle_t h, int granted) {
  imobs_set_fetch_consent(h, granted);
}
madobs_status_t madobs_crash_install(const char* marker_path) {
  return imobs_crash_install(marker_path);
}
int32_t madobs_crash_consume_pending(madobs_handle_t h,
                                     const char* marker_path) {
  return imobs_crash_consume_pending(h, marker_path);
}
int32_t madobs_crash_in_loop(const char* marker_path, uint32_t threshold,
                             int64_t window_ms, int64_t now_wall_ms) {
  return imobs_crash_in_loop(marker_path, threshold, window_ms, now_wall_ms);
}
uint64_t madobs_name_hash(const char* name) { return imobs_name_hash(name); }
madobs_status_t madobs_get_self_stats(madobs_handle_t h,
                                      madobs_self_stats_t* out) {
  return imobs_get_self_stats(h, out);
}

madobs_private_batch_result madobs_check_private_batch(
    const uint8_t* data, size_t len, const madobs_privacy_policy_v1* policy) {
  if (!madobs_privacy_policy_valid(policy)) return MADOBS_PRIVATE_BAD_POLICY;
  if (!data || len > 64u * 1024u * 1024u) return MADOBS_PRIVATE_BAD_WIRE;
  try {
    imobs::BatchView batch;
    bool resource_failure = false;
    if (!imobs::ParseBatchViewDetailed(data, len, &batch, &resource_failure))
      return resource_failure ? MADOBS_PRIVATE_RESOURCE_FAILURE
                              : MADOBS_PRIVATE_BAD_WIRE;
    if (batch.records.empty() || batch.pri > 2) return MADOBS_PRIVATE_BAD_WIRE;
    std::vector<uint8_t> frames;
    imobs::WireWriter frame_writer(&frames);
    for (const auto& record : batch.records) {
      madobs_event_v1 event{}, clean{};
      if (madobs_decode_event_record(
              reinterpret_cast<const uint8_t*>(record.data()), record.size(),
              &event, sizeof(event)) != MADOBS_OK ||
          madobs_sanitize_event(policy, &event, &clean) != MADOBS_EMITTED ||
          event.sdk_id != clean.sdk_id ||
          event.sdk_version != clean.sdk_version ||
          event.event_id != clean.event_id ||
          event.severity != clean.severity ||
          event.monotonic_ns != clean.monotonic_ns ||
          event.attribute_count != clean.attribute_count)
        return MADOBS_PRIVATE_UNSAFE_RECORD;
      for (uint32_t i = 0; i < event.attribute_count; ++i)
        if (event.attributes[i].key != clean.attributes[i].key ||
            event.attributes[i].enum_value != clean.attributes[i].enum_value)
          return MADOBS_PRIVATE_UNSAFE_RECORD;
      std::vector<uint8_t> canonical;
      imobs::EncodeGenericEvent(clean, &canonical);
      if (canonical.size() != record.size() ||
          std::memcmp(canonical.data(), record.data(), record.size()) != 0)
        return MADOBS_PRIVATE_NONCANONICAL;
      frame_writer.Varint(canonical.size());
      frame_writer.Bytes(canonical.data(), canonical.size());
    }
    // Rebuild the exact canonical wire envelope, including compression. This
    // rejects ignored zlib suffixes, nonminimal varints, bad IDs and covert
    // bytes. A compressor upgrade may reject old compressed records, safely,
    // not leak.
    std::vector<uint8_t> body, compressed, full;
    imobs::WireWriter w(&body);
    w.Bytes(batch.compressed ? "IMBZ" : "IMB1", 4);
    w.U8(batch.pri);
    w.Fixed64(batch.anchor_wall_ms);
    w.Fixed64(batch.anchor_mono_ns);
    w.Varint(batch.records.size());
    if (batch.compressed) {
      const auto result = imobs::DeflateBytesDetailed(
          frames.data(), frames.size(), &compressed);
      if (result != imobs::CodecResult::kOk)
        return result == imobs::CodecResult::kResourceFailure
                   ? MADOBS_PRIVATE_RESOURCE_FAILURE
                   : MADOBS_PRIVATE_NONCANONICAL;
      w.Varint(frames.size());
      w.Bytes(compressed.data(), compressed.size());
    } else
      w.Bytes(frames.data(), frames.size());
    imobs::WireWriter out(&full);
    out.Fixed64(imobs::Fnv1a64(body.data(), body.size()));
    out.Bytes(body.data(), body.size());
    return full.size() == len && std::memcmp(full.data(), data, len) == 0
               ? MADOBS_PRIVATE_BATCH_OK
               : MADOBS_PRIVATE_NONCANONICAL;
  } catch (...) {
    return MADOBS_PRIVATE_RESOURCE_FAILURE;
  }
}

int madobs_validate_private_batch(const uint8_t* data, size_t len,
                                  const madobs_privacy_policy_v1* policy) {
  return madobs_check_private_batch(data, len, policy) ==
         MADOBS_PRIVATE_BATCH_OK;
}
