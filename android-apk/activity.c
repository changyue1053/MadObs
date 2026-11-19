/* Link/package fixture only. No WAL, worker, sink, credential or network endpoint. */
#include <android/native_activity.h>
#include <madobs/runtime.h>
void ANativeActivity_onCreate(ANativeActivity *activity, void *state, size_t size) {
  (void)activity; (void)state; (void)size;
  madobs_generic_stats_v1 stats = {0};
  /* Exercise a real runtime symbol's invalid-handle path, without creating runtime. */
  (void)madobs_get_generic_stats(0, &stats);
}
