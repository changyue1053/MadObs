#define CHECK(x)               \
  do {                         \
    if (!(x)) return __LINE__; \
  } while (0)
#include <stdio.h>
#include <stdlib.h>

#include "madobs/private.h"
static void Send(void* ctx, uint8_t pri, uint64_t id, const uint8_t* bytes,
                 size_t n, madobs_send_done_fn done, void* dc) {
  (void)ctx;
  (void)pri;
  (void)id;
  (void)bytes;
  (void)n;
  done(dc, 1);
}
int main(void) {
  char parent[] = "/tmp/madobs_private_c_XXXXXX", path[128];
  CHECK(mkdtemp(parent));
  snprintf(path, sizeof(path), "%s/wal", parent);
  madobs_event_rule_v1 rule = {0};
  rule.event_id = 3;
  madobs_privacy_policy_v1 policy = {sizeof(policy), 1, 1, &rule};
  madobs_private_open_result reason = MADOBS_PRIVATE_OPEN_INTERNAL;
  madobs_private_handle_t handle =
      madobs_private_open_ex(path, Send, NULL, &policy, &reason);
  CHECK(reason == MADOBS_PRIVATE_OPEN_OK);
  CHECK(handle);
  madobs_event_v1 event = {0};
  event.struct_size = sizeof(event);
  event.abi_version = 1;
  event.event_id = 3;
  CHECK(madobs_private_emit(handle, &event) == MADOBS_OK);
  event.event_id = 4;
  CHECK(madobs_private_emit(handle, &event) == MADOBS_E_SCHEMA_REJECTED);
  CHECK(madobs_private_stop(handle) == MADOBS_OK);
  madobs_generic_stats_v1 stats;
  CHECK(madobs_private_get_stats(handle, &stats, sizeof(stats)) == MADOBS_OK &&
        stats.drained == 1);
  CHECK(madobs_private_get_stats(handle, &stats, sizeof(stats) - 1) ==
        MADOBS_E_INVALID_ARG);
  madobs_private_gate_stats_v1 gate;
  CHECK(madobs_private_get_gate_stats(handle, &gate, sizeof(gate)) ==
            MADOBS_OK &&
        gate.rejected_attempts == 0);
  CHECK(madobs_private_destroy(handle) == MADOBS_OK);
  CHECK(madobs_private_emit(handle, &event) == MADOBS_E_INVALID_HANDLE);
  CHECK(madobs_private_destroy(handle) == MADOBS_E_INVALID_HANDLE);
  madobs_private_handle_t second =
      madobs_private_open(path, Send, NULL, &policy);
  CHECK(second && second != handle);
  CHECK(madobs_private_get_gate_stats(handle, &gate, sizeof(gate)) ==
        MADOBS_E_INVALID_HANDLE);
  CHECK(madobs_private_destroy(second) == MADOBS_OK);
  return 0;
}
