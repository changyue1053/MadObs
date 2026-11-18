#define CHECK(expr)               \
  do {                            \
    if (!(expr)) return __LINE__; \
  } while (0)
#include <stdlib.h>

#include "madobs/privacy.h"
#include "madobs/runtime.h"
static void send_batch(void* ctx, uint8_t p, uint64_t id, const uint8_t* bytes,
                       size_t len, madobs_send_done_fn done, void* dc) {
  (void)ctx;
  (void)p;
  (void)id;
  (void)bytes;
  (void)len;
  done(dc, 1);
}
int main(void) {
  char dir[] = "/tmp/madobs_installed_XXXXXX";
  CHECK(mkdtemp(dir));
  madobs_handle_t h = madobs_create(dir, send_batch, NULL);
  CHECK(h);
  CHECK(madobs_start(h) == MADOBS_OK);
  madobs_observer_binding_v1 binding;
  madobs_observer_v1 observer;
  CHECK(madobs_observer_for_runtime(h, &binding, &observer) == MADOBS_OK);
  madobs_event_v1 event = {0};
  event.struct_size = sizeof(event);
  event.abi_version = 1;
  event.severity = 4;
  CHECK(madobs_observer_emit(&observer, &event) == MADOBS_EMITTED);
  madobs_event_rule_v1 rule = {0};
  rule.max_severity = 4;
  madobs_privacy_policy_v1 policy = {sizeof(policy), MADOBS_ABI_VERSION, 1,
                                     &rule};
  event.monotonic_ns = 123456;
  event.attribute_count = 1;
  event.attributes[0].key = 999; /* deliberately unregistered: removed */
  event.attributes[0].enum_value = 12345;
  CHECK(madobs_observer_emit_private(&observer, &policy, &event) ==
        MADOBS_EMITTED);
  CHECK(madobs_stop(h) == MADOBS_OK);
  madobs_generic_stats_v1 stats;
  CHECK(madobs_get_generic_stats(h, &stats) == MADOBS_OK && stats.drained == 2);
  CHECK(madobs_destroy(h) == MADOBS_OK);
  return 0;
}
