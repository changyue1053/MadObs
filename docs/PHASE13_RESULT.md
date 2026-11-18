# Thirteenth increment: bounded deterministic boundary properties

Status: regression-test and documentation change only; production codec/privacy
behavior unchanged. This is not coverage-guided fuzzing or a safety certification.

## Exact scope and reproduction

`test_madobs_boundary_properties` runs 10,000 cases with fixed xorshift32 seed
`0x49a7f421`. Failures print the seed, iteration, generator state and source line;
rerun the same executable/source to reproduce. Bounds: one rule, 0..8 dimensions,
1..16 allowed enum values per dimension, 0..8 event attributes, four single-byte
wire mutations per generated event, every truncated prefix, and one random byte
buffer of length 0..160 per iteration (half with G1 magic forced). Explicit cases
cover maximum uint32/uint64 values and maximum arity (107-byte valid G1 record).

Assertions cover:

- Sanitization agrees with the generated finite-domain expected result, removes
  timing/inactive slots, and has consistent in-place and idempotent behavior.
- Severity/duplicate-key rejection preserves the prior logical sanitized result.
- Valid codec round trips preserve active logical fields; inactive decoded slots
  are zero. Every truncated prefix and an appended byte reject.
- Rejected arbitrary/mutated/prefix buffers leave the entire decoder output
  unchanged. Successful mutated/random inputs produce valid, bounded re-encodable
  records with a stable logical round trip.
- A specific non-minimal varint remains accepted by raw decoding and normalizes
  on re-encoding. This deliberately preserves existing raw compatibility, not
  approval by the separate canonical private export gate.

Run `ctest --test-dir <build> -R madobs_boundary_properties --output-on-failure`.
The standard native CI includes this bounded test; it does not require a separate
fuzzer toolchain. Existing private-gate tests remain the policy/canonical-envelope
checks. `GENERIC_WIRE_V1.md` now describes implemented optional schema protection
and explicitly distinguishes raw structural decoding from private gate approval.

## Verification and environment incident

During sequential full-suite runs the shared /tmp tmpfs was observed 100% full
(4.9GB). BoringSSL and host-JVM runs reported SIGBUS and other failures. Preserve
these as failed runs, not passes:

- verification/phase13-boringssl-full-tmp-failure.log
- verification/phase13-jni-full-tmp-failure.log

No other SDK files/cache/evidence were deleted. Tests use some hard-coded /tmp
paths despite TMPDIR. Re-runs used bwrap to give each test suite a fresh private
/tmp backed by this project's shared-disk directory. The same BoringSSL binaries
then passed 44/44. This is evidence consistent with an environment-capacity
failure, not proof that every SIGBUS can only have that cause.

Final complete isolated-/tmp matrices (verification/phase13-final-*.log):

- Native 43/43; Release 43/43; real provider 44/44; API-only 6/6.
- ASan+UBSan 42/42 with leak detection disabled; TSan 42/42. The new property test
  is compiled with the respective sanitizer flags. No LSan/JVM-sanitizer claim.
- Host-JVM null provider 46/46; host-JVM real provider 47/47.

The private /tmp namespace affects only the test process, not other workers or
system settings. No new SDK license was accepted. Mobile toolchains/devices,
complete input-space exploration, real resource-exhaustion recovery and production
certification remain outside this evidence.
