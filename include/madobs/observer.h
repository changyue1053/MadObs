#ifndef MADOBS_OBSERVER_H
#define MADOBS_OBSERVER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MADOBS_ABI_VERSION 1u
#define MADOBS_MAX_ATTRIBUTES 8u
/* No user strings, payloads, paths or identifiers in v1 attributes.
 * IDs refer to compile-time SDK schemas; enum values must have finite domains.
 * Trace correlation requires an explicit host privacy policy, disabled in v1.
 */
typedef struct {
  uint32_t key;
  uint32_t enum_value;
} madobs_attribute_v1;
typedef struct {
  uint32_t struct_size, abi_version;
  uint32_t sdk_id, sdk_version, event_id, severity;
  uint64_t monotonic_ns;
  uint32_t attribute_count, reserved;
  madobs_attribute_v1 attributes[MADOBS_MAX_ATTRIBUTES];
} madobs_event_v1;
/* Callback borrows event only for the call, must not block, retain pointers, or
 * throw. API catches C++ exceptions, but cannot catch faults or C longjmp.
 * Host owns ctx. Before replacing/freeing it, stop and join all producers.
 * v1 does NOT support concurrent observer unregistration. */
typedef struct {
  uint32_t struct_size, abi_version;
  void* ctx;
  void (*emit)(void*, const madobs_event_v1*);
} madobs_observer_v1;
/* EMITTED means callback invoked, NOT accepted, persisted or exported.
 * Inspect runtime counters or call madobs_emit_generic_event for admission
 * status. */
typedef enum {
  MADOBS_EMITTED = 0,
  MADOBS_NOOP = 1,
  MADOBS_INVALID = 2,
  MADOBS_CALLBACK_ERROR = 3
} madobs_emit_result;
static inline madobs_observer_v1 madobs_noop_observer(void) {
  madobs_observer_v1 r = {sizeof(madobs_observer_v1), MADOBS_ABI_VERSION, NULL,
                          NULL};
  return r;
}
static inline madobs_emit_result madobs_observer_emit(
    const madobs_observer_v1* observer, const madobs_event_v1* event) {
  if (!observer) return MADOBS_NOOP;
  if (observer->struct_size < sizeof(*observer) ||
      observer->abi_version != MADOBS_ABI_VERSION)
    return MADOBS_INVALID;
  if (!observer->emit) return MADOBS_NOOP;
  if (!event || event->struct_size < sizeof(*event) ||
      event->abi_version != MADOBS_ABI_VERSION ||
      event->attribute_count > MADOBS_MAX_ATTRIBUTES || event->severity > 5 ||
      event->reserved != 0)
    return MADOBS_INVALID;
#if defined(__cplusplus) && (defined(__cpp_exceptions) || \
                             defined(__EXCEPTIONS) || defined(_CPPUNWIND))
  try {
    observer->emit(observer->ctx, event);
  } catch (...) {
    return MADOBS_CALLBACK_ERROR;
  }
#else
  observer->emit(observer->ctx, event);
#endif
  return MADOBS_EMITTED;
}
#ifdef __cplusplus
}
#endif
#endif
