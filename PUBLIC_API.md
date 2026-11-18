# Public API v0.1 (experimental, not frozen)

- `include/madobs/observer.h`: C-compatible ABI version and struct_size fields.
- `madobs_noop_observer`, `madobs_observer_emit`: API-only, no linking runtime required.
- Event fields: SDK numeric schema/version, event ID, severity, monotonic timestamp,
  maximum eight numeric enum attributes. Reserved fields must be zero. No free-text,
  arbitrary path, model prompt, user identifier or message body field in this version.
- Unknown ABI or too-small structs reject with INVALID. Tail extension is tolerated.
- Host emit callback must be bounded/nonblocking. The SDK cannot make a hostile or slow
  custom callback nonblocking; SDK adapters must enqueue rather than perform IO.
- Context borrowed from host; all producers must stop/join before replacement/free.
  No runtime lease/unregister-drain API is implemented yet.
- C++ callback exceptions return CALLBACK_ERROR; C callers may not unwind/longjmp.
- Runtime still exports legacy imobs_ compatibility symbols. Repeated stop is OK;
  repeated destroy returns INVALID_HANDLE. No APIs may race lifecycle destruction.
- Healer completions may arrive on any thread; state changes happen in Tick, not inline.
  Late callbacks after Healer destruction are discarded; mailbox capacity 1024,
  overflow counted in Healer stats and action timeout remains the recovery path.

## Phase 2: runtime ABI and generic bridge

- `madobs/runtime.h` exports real `madobs_` C symbols, not preprocessor renames.
  The legacy `imobs_` ABI remains available; bundled compatibility typedefs keep
  layouts identical. No external SDK is needed for these compatibility types.
- `madobs_observer_for_runtime` fills a Host-owned binding plus observer. Binding
  storage must remain alive and immutable while producers run. It may be shared
  across producer threads. No heap or thread is created by binding.
- `madobs_emit_generic_event` validates and copies an event into a bounded 1024-slot
  queue; CAS retries are bounded at 64 and overload returns QUEUE_FULL. No IO,
  mutex, or dynamic allocation on this producer path.
- `MADOBS_EMITTED` from the header helper means callback **invoked**, not admission,
  persistence or remote acknowledgement. Use the direct emit status for admission.
- Shutdown is explicit: stop/join all producers, stop runtime, destroy runtime,
  then release binding/context. Concurrent emit vs stop/destroy is unsupported.
  A sequential stale runtime handle through a still-live binding is safely rejected.
- `stop` is terminal. Running-instance `start` is idempotent; `start` after `stop`
  returns BAD_STATE. Restart with a fresh instance using the same WAL directory.
- `madobs_get_generic_stats` returns atomic counters: enqueued, drained,
  queue_dropped, wal_dropped. Concurrent snapshots are not transactionally consistent.
  `wal_dropped` counts generic records rejected on WAL append, including delayed
  P1 flush; it is NOT total historical loss and excludes configured old-segment
  overwrites, crash-before-buffer-flush, torn recovery records or server loss.
- Generic events are serialized as new opt-in G1 records within existing batch/WAL
  framing. All eight attributes, SDK ID/version, severity and timestamp are retained.
  Severity >=4 routes to P0; lower values to P1. Host must understand G1 records;
  the retained legacy thin-server query model is not a generic event query backend.
- `madobs_decode_event_record` decodes one record after batch decompression/framing,
  validates versions/lengths/overflow, and leaves output unchanged on failure.
- Runtime's installed static package is `MadObs::runtime`. C source consumers enable
  both C and C++ toolchains for the native implementation. API-only still needs only C.
- The observer header supports `-fno-exceptions`; in that configuration callbacks
  must not throw (there is no exception runtime to catch them).

Stop flushes local buffers and attempts export; it does not guarantee remote ACK.
Host exporters must be nonblocking and complete exactly once.

## Optional finite-domain privacy helper (additive)

`madobs/privacy.h` is header-only and installed with MadObs::api. No existing ABI
symbol is replaced. `madobs_sanitize_event` and `madobs_observer_emit_private`
validate a host-owned immutable allowlist and minimize permitted events before
emission. Raw emit and legacy logs do NOT automatically apply this policy.
See docs/PRIVACY_SCHEMA.md for exact reject/drop/lifetime semantics and limitations.

## Optional owning private runtime

`madobs/private_runtime.hpp` adds a C++11 ownership binding with deep-copied schema
and synchronized Emit/Close. It permits WAL recovery only with an exact versioned policy identity and validates
every exported batch against that policy; raw/unknown/changed-policy storage fails closed.
Raw ABI is unchanged. See `docs/PRIVATE_RUNTIME.md` and latest `docs/PHASE5_RESULT.md`.
This native substrate is not a validated JNI/Swift/ArkTS managed package.

`madobs_validate_private_batch` is an additive exported C function for fail-closed
canonical batch validation, used by PrivateRuntime. Its return value is validation
only, not enqueue/transport completion. See PRIVATE_RUNTIME.md for codec/order and
batch clock visibility limitations. Raw legacy C APIs are unchanged.

`madobs/private.h` is the owning native C binding; stop keeps a handle for final
stats, destroy revokes it. `madobs_check_private_batch` provides explicit diagnostic
codes, and gate stats provide numeric rejection history only. See
`docs/BLOCKED_WAL_RUNBOOK.md` and `docs/PHASE6_RESULT.md`. Neither diagnostic history
nor an INVALID_HANDLE from a competing destroy establishes drain completion.

`madobs_private_open_ex` adds optional coarse opening diagnostics while legacy
`madobs_private_open` remains compatible. See the runbook for safe bounded retry
and why mismatch/UNBOUND never authorize rewriting identities. Installed CMake
metadata accepts only an equal version when requested, while unversioned discovery
remains compatible (pinning is recommended, not mandatory); see `scripts/verify-installed.sh`.
