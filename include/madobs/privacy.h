#ifndef MADOBS_PRIVACY_H
#define MADOBS_PRIVACY_H
#include "madobs/observer.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MADOBS_SCHEMA_MAX_EVENTS 64u
#define MADOBS_SCHEMA_MAX_VALUES 16u
/* Immutable, host-owned compile-time allowlist. Never construct from user data.
 * No global registry, allocations, hashing or string processing. */
typedef struct {
  uint32_t key, value_count;
  uint32_t values[MADOBS_SCHEMA_MAX_VALUES];
} madobs_attribute_rule_v1;
typedef struct {
  uint32_t sdk_id, sdk_version, event_id, max_severity, attribute_count;
  madobs_attribute_rule_v1 attributes[MADOBS_MAX_ATTRIBUTES];
} madobs_event_rule_v1;
typedef struct {
  uint32_t struct_size, abi_version, event_count;
  const madobs_event_rule_v1* events;
} madobs_privacy_policy_v1;
/* Validate the entire policy, not just the matched rule. Empty policy denies
 * all. Duplicate event identities/attribute keys/enum values are configuration
 * errors. */
static inline int madobs_privacy_policy_valid(
    const madobs_privacy_policy_v1* p) {
  uint32_t i, j, k, l;
  if (!p || p->struct_size < sizeof(*p) ||
      p->abi_version != MADOBS_ABI_VERSION ||
      p->event_count > MADOBS_SCHEMA_MAX_EVENTS ||
      (p->event_count && !p->events))
    return 0;
  for (i = 0; i < p->event_count; ++i) {
    const madobs_event_rule_v1* e = &p->events[i];
    if (e->max_severity > 5 || e->attribute_count > MADOBS_MAX_ATTRIBUTES)
      return 0;
    for (j = 0; j < i; ++j)
      if (e->sdk_id == p->events[j].sdk_id &&
          e->sdk_version == p->events[j].sdk_version &&
          e->event_id == p->events[j].event_id)
        return 0;
    for (j = 0; j < e->attribute_count; ++j) {
      const madobs_attribute_rule_v1* a = &e->attributes[j];
      if (!a->value_count || a->value_count > MADOBS_SCHEMA_MAX_VALUES)
        return 0;
      for (k = 0; k < j; ++k)
        if (a->key == e->attributes[k].key) return 0;
      for (k = 0; k < a->value_count; ++k)
        for (l = 0; l < k; ++l)
          if (a->values[k] == a->values[l]) return 0;
    }
  }
  return 1;
}
/* Fail closed for malformed/unknown events, severity and duplicate attributes.
 * Drop unknown keys and out-of-domain values. Strip exact timing and inactive
 * slots. Failure leaves out unchanged; in==out is supported. This is allowlist
 * minimization, NOT anonymization of legacy raw logs or a substitute for
 * consent. */
static inline madobs_emit_result madobs_sanitize_event(
    const madobs_privacy_policy_v1* p, const madobs_event_v1* in,
    madobs_event_v1* out) {
  uint32_t i, j, k;
  const madobs_event_rule_v1* rule = NULL;
#ifdef __cplusplus
  madobs_event_v1 clean{};
#else
  madobs_event_v1 clean = {0};
#endif
  if (!out || !in || in->struct_size < sizeof(*in) ||
      in->abi_version != MADOBS_ABI_VERSION || in->reserved ||
      in->attribute_count > MADOBS_MAX_ATTRIBUTES || in->severity > 5 ||
      !madobs_privacy_policy_valid(p))
    return MADOBS_INVALID;
  for (i = 0; i < p->event_count; ++i)
    if (p->events[i].sdk_id == in->sdk_id &&
        p->events[i].sdk_version == in->sdk_version &&
        p->events[i].event_id == in->event_id)
      rule = &p->events[i];
  if (!rule || in->severity > rule->max_severity) return MADOBS_INVALID;
  clean.struct_size = sizeof(clean);
  clean.abi_version = MADOBS_ABI_VERSION;
  clean.sdk_id = rule->sdk_id;
  clean.sdk_version = rule->sdk_version;
  clean.event_id = rule->event_id;
  clean.severity = in->severity;
  for (i = 0; i < in->attribute_count; ++i) {
    for (j = 0; j < i; ++j)
      if (in->attributes[i].key == in->attributes[j].key) return MADOBS_INVALID;
    for (j = 0; j < rule->attribute_count; ++j)
      if (in->attributes[i].key == rule->attributes[j].key) {
        for (k = 0; k < rule->attributes[j].value_count; ++k)
          if (in->attributes[i].enum_value == rule->attributes[j].values[k]) {
            clean.attributes[clean.attribute_count++] = in->attributes[i];
            break;
          }
      }
  }
  *out = clean;
  return MADOBS_EMITTED; /* sanitized only, no callback or IO has occurred */
}
static inline madobs_emit_result madobs_observer_emit_private(
    const madobs_observer_v1* observer, const madobs_privacy_policy_v1* policy,
    const madobs_event_v1* event) {
  madobs_event_v1 clean;
  if (!observer) return MADOBS_NOOP;
  if (observer->struct_size < sizeof(*observer) ||
      observer->abi_version != MADOBS_ABI_VERSION)
    return MADOBS_INVALID;
  if (!observer->emit) return MADOBS_NOOP;
  if (madobs_sanitize_event(policy, event, &clean) != MADOBS_EMITTED)
    return MADOBS_INVALID;
  return madobs_observer_emit(observer, &clean);
}
#ifdef __cplusplus
}
#endif
#endif
