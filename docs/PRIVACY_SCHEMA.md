# v1 finite-domain privacy boundary

`madobs/privacy.h` adds an optional header-only policy layer without changing old
ABI, G1 wire format, or legacy logging. No registry is built from telemetry/user data.
Host-approved immutable policies identify exact SDK id/version/event id, maximum
severity, at most eight attribute keys and up to sixteen distinct enum values per
key. There are at most 64 event rules. Duplicate rules/keys/values are rejected.
The host must ensure these constants encode safe categories, not user identifiers.
Policies are not authenticated remote configuration and must not come from users.

Empty/missing policy denies real emissions. Noop observers still do nothing without
requiring a policy. Unknown event/version, bad severity or duplicate input keys fail
closed without callback. Unknown attribute keys and out-of-domain enum values are
dropped. Timing is always zeroed, inactive slots are zeroed, reserved is zero, and
output structure is rebuilt rather than copying hidden/stale fields. On failure,
caller output stays unchanged, including in-place operation. Arrays/pointers supplied
by C callers must still reference valid storage for their advertised size.

Use `madobs_sanitize_event` before direct runtime submission, or
`madobs_observer_emit_private` to sanitize and invoke an injected observer. The
sanitizer's MADOBS_EMITTED means successful sanitization only; the wrapper result
means callback dispatch, neither guarantees queue admission nor delivery. The
callback sees a temporary borrowed object, not the caller's object.

This is deterministic field minimization, not anonymization, encryption, sampling,
user consent or complete PII scanning. Do not put PII in sdk/event ids, severity or
schema enum constants. Event occurrence/count and batch/network timing still reveal
behavior. Legacy raw logs, names, paths, crash frames and the unrestricted old/new
raw emit entrypoints bypass this layer by design for compatibility. Hosts requiring
a hard privacy boundary must not expose/use those entrypoints or enable raw logs.

No cross-SDK schemas were invented: registration requires each SDK's reviewed event
inventory. No other SDK is pulled into MadObs. Admission errors currently return to
the caller; no global privacy-drop counter or per-policy rate limiting is claimed.
Policies are revalidated on each call with bounded loops; benchmark on target before
putting large policies on high-frequency paths. No runtime allocation/IO is added.
