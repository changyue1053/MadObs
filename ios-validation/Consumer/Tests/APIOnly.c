/* Header-only consumer: no runtime library, filesystem, threads, or network. */
#include "madobs/privacy.h"
#include <string.h>

static void capture(void *context, const madobs_event_v1 *event) {
    *(madobs_event_v1 *)context = *event;
}
int madobs_api_only_contract(void) {
    madobs_event_v1 event = {0}, observed = {0};
    event.struct_size = sizeof(event); event.abi_version = MADOBS_ABI_VERSION;
    event.sdk_id = 42; event.sdk_version = 1; event.event_id = 7; event.severity = 4;
    event.monotonic_ns = 123; event.attribute_count = 2;
    event.attributes[0] = (madobs_attribute_v1){8, 1};
    event.attributes[1] = (madobs_attribute_v1){99, 2};
    madobs_observer_v1 observer = {sizeof(observer), MADOBS_ABI_VERSION, &observed, capture};
    madobs_observer_v1 noop = madobs_noop_observer();
    if (madobs_observer_emit(&noop, &event) != MADOBS_NOOP) return 1;
    if (madobs_observer_emit(&observer, &event) != MADOBS_EMITTED) return 2;
    if (observed.sdk_id != 42 || observed.attributes[1].enum_value != 2) return 3;
    madobs_event_rule_v1 rule = {0};
    rule.sdk_id = 42; rule.sdk_version = 1; rule.event_id = 7; rule.max_severity = 4;
    rule.attribute_count = 1; rule.attributes[0].key = 8;
    rule.attributes[0].value_count = 1; rule.attributes[0].values[0] = 1;
    madobs_privacy_policy_v1 policy = {sizeof(policy), MADOBS_ABI_VERSION, 1, &rule};
    if (madobs_observer_emit_private(&observer, &policy, &event) != MADOBS_EMITTED) return 4;
    if (observed.monotonic_ns != 0 || observed.attribute_count != 1 ||
        observed.attributes[0].key != 8 || observed.attributes[0].enum_value != 1) return 5;
    event.event_id = 100;
    if (madobs_observer_emit_private(&observer, &policy, &event) != MADOBS_INVALID) return 6;
    return 0;
}
#ifdef MADOBS_API_ONLY_MAIN
int main(void) { return madobs_api_only_contract(); }
#endif
