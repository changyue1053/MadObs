#include <assert.h>
#include <string.h>

#include "madobs/privacy.h"
static madobs_event_v1 captured;
static unsigned calls;
static void receive(void* ctx, const madobs_event_v1* e) {
  (void)ctx;
  captured = *e;
  ++calls;
}
int main(void) {
  madobs_event_rule_v1 rule = {0};
  madobs_privacy_policy_v1 p = {sizeof(p), MADOBS_ABI_VERSION, 1, &rule};
  madobs_event_v1 in = {0}, out = {0}, saved;
  madobs_observer_v1 sink = {sizeof(sink), MADOBS_ABI_VERSION, 0, receive};
  rule.sdk_id = 2;
  rule.sdk_version = 1;
  rule.event_id = 7;
  rule.max_severity = 3;
  rule.attribute_count = 1;
  rule.attributes[0].key = 11;
  rule.attributes[0].value_count = 2;
  rule.attributes[0].values[0] = 1;
  rule.attributes[0].values[1] = 3;
  in.struct_size = sizeof(in);
  in.abi_version = MADOBS_ABI_VERSION;
  in.sdk_id = 2;
  in.sdk_version = 1;
  in.event_id = 7;
  in.severity = 2;
  in.monotonic_ns = 123456789;
  in.attribute_count = 2;
  in.attributes[0].key = 11;
  in.attributes[0].enum_value = 3;
  in.attributes[1].key = 12345;
  in.attributes[1].enum_value = 987654;
  in.attributes[7].key = 0xdeadbeef;
  assert(madobs_privacy_policy_valid(&p));
  assert(madobs_observer_emit_private(&sink, &p, &in) == MADOBS_EMITTED);
  assert(calls == 1 && captured.monotonic_ns == 0 &&
         captured.attribute_count == 1);
  assert(captured.attributes[0].key == 11 &&
         captured.attributes[0].enum_value == 3);
  for (unsigned i = 1; i < 8; ++i)
    assert(!captured.attributes[i].key && !captured.attributes[i].enum_value);
  in.attributes[0].enum_value = 999;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_EMITTED &&
         out.attribute_count == 0);
  saved = out;
  in.event_id = 8;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_INVALID &&
         !memcmp(&out, &saved, sizeof(out)));
  in.event_id = 7;
  in.sdk_version = 2;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_INVALID);
  in.sdk_version = 1;
  in.severity = 4;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_INVALID);
  in.severity = 2;
  in.attributes[1].key = 11;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_INVALID);
  in.attribute_count = 1;
  in.attributes[0].enum_value = 1;
  assert(madobs_sanitize_event(&p, &in, &in) == MADOBS_EMITTED &&
         in.monotonic_ns == 0);
  rule.attributes[0].value_count = 17;
  assert(!madobs_privacy_policy_valid(&p));
  rule.attributes[0].value_count = 2;
  rule.attributes[0].values[1] = 1;
  assert(!madobs_privacy_policy_valid(&p));
  rule.attributes[0].values[1] = 3;
  rule.attribute_count = 2;
  rule.attributes[1] = rule.attributes[0];
  assert(!madobs_privacy_policy_valid(&p));
  rule.attribute_count = 1;
  madobs_event_rule_v1 duplicate[2] = {rule, rule};
  p.events = duplicate;
  p.event_count = 2;
  assert(!madobs_privacy_policy_valid(&p));
  p.event_count = 65;
  assert(!madobs_privacy_policy_valid(&p));
  p.event_count = 0;
  p.events = 0;
  assert(madobs_sanitize_event(&p, &in, &out) == MADOBS_INVALID);
  assert(madobs_observer_emit_private(0, 0, 0) == MADOBS_NOOP);
  madobs_observer_v1 noop = madobs_noop_observer();
  assert(madobs_observer_emit_private(&noop, 0, 0) == MADOBS_NOOP);
  assert(calls == 1);
  return 0;
}
