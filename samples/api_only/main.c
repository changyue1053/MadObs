#include "madobs/privacy.h"
/* A reusable SDK owns an optional observer value, defaulting to no-op. */
int main(void) {
  madobs_observer_v1 observer = madobs_noop_observer();
  madobs_event_v1 event = {0};
  event.struct_size = sizeof(event);
  event.abi_version = MADOBS_ABI_VERSION;
  event.sdk_id = 1;
  event.sdk_version = 1;
  event.event_id = 1;
  return madobs_observer_emit_private(&observer, NULL, &event) == MADOBS_NOOP
             ? 0
             : 1;
}
