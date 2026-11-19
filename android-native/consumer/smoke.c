/* Link fixture only in this checkpoint. No Android execution is performed. */
#include <madobs/runtime.h>
#include <stdatomic.h>
static atomic_uint batches;
static void sink(void* context, uint8_t type, uint64_t sequence,
                 const uint8_t* data, size_t size, madobs_send_done_fn done, void* done_context) {
  (void)context; (void)type; (void)sequence; (void)data; (void)size;
  atomic_fetch_add(&batches, 1); done(done_context, 1);
}
int main(int argc, char** argv) {
  /* A caller must supply its own disposable local WAL directory; no endpoint. */
  if (argc != 2) return 2;
  madobs_handle_t h = madobs_create(argv[1], sink, 0);
  if (!h) return 3;
  int result = 0;
  if (madobs_start(h) != MADOBS_OK) result = 4;
  madobs_event_v1 event = {0};
  event.struct_size = sizeof(event); event.abi_version = MADOBS_ABI_VERSION;
  event.sdk_id = 1; event.sdk_version = 1; event.event_id = 1;
  if (!result && madobs_emit_generic_event(h, &event) != MADOBS_OK) result = 5;
  /* Sole producer is now quiescent; stop joins workers before sink inspection. */
  if (madobs_stop(h) != MADOBS_OK && !result) result = 6;
  madobs_generic_stats_v1 stats = {0};
  if (!result && (madobs_get_generic_stats(h, &stats) != MADOBS_OK ||
      stats.enqueued != 1 || stats.drained != 1 || stats.queue_dropped ||
      stats.wal_dropped || !atomic_load(&batches))) result = 7;
  if (madobs_destroy(h) != MADOBS_OK && !result) result = 8;
  return result;
}
