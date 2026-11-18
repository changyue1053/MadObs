# MadObs first-stage local result

Latest: see docs/PHASE2_RESULT.md for implemented generic bridge/Mad ABI and current tests.
The phase-1 gaps below are historical where superseded there.

Task: A00/A01/A02/O01/O02 local first increment
Status: PARTIAL overall; native baseline and focused safety fixes VERIFIED.
No push, PR, release, remote mutation, IMSDK work or Codex cloud development task.

## Baseline

- Source: changyue1053/IMObs @ f88c3531e9b1e824c8b176458acace8fd4ec41b2.
- Target: changyue1053/MadObs, verified public and empty (commits endpoint 409).
- 86 imported source/build/test files; every Git blob SHA verified.
- Exact local baseline: 461b6797aca2f97fe619e11f3bbb7ded54b2ab56.
- Initial materialization added an extra terminal LF; all 86 were corrected against
  source hashes before final validation. The initial replay-golden mismatch was a
  materialization artifact, NOT a source defect.
- Existing user workspace was empty at this path; no pre-existing changes overwritten.
- Toolchain: Linux x86_64, GCC 14.2.0, CMake 4.4.5 (temporary PyPI tool), system zlib.

## Implemented

1. Preserve native core, source tests, crash handling, logs/WAL, metrics, traces,
   policy, healing, tools. Explicitly preserve wire/disk formats and imobs_ ABI.
2. Independent root build; no sibling IMDB/IMLink discovery or build requirement.
3. Header-only madobs-api C/C++ contract, ABI/size checks, default Noop,
   bounded numeric attributes, exception firewall for C++ custom observers.
4. API-only build, installed CMake package and fresh C-only consumer sample.
5. Healer async completions use weak-lifetime bounded queue; only Tick mutates
   state. Late callbacks are discarded, overflow counted, no exception escapes
   completion enqueue. Existing timing tests adapted to explicit Tick semantics.
6. Firewall no longer blocks stop/destroy cleanup.
7. Remote control defaults disabled; empty and historical development keys cannot
   apply submitted envelopes. Control inbox size and envelope length bounded.
8. ELF version-script + portable symbol audit; crash-test frame-chain fixture uses
   synchronous trap with frame pointers, avoiding libc raise frame assumptions.

## Verification (final code)

Commands use `cmake` / `ctest` 4.4.5 binaries from the temporary tool installation.

| Command/configuration | Result |
| --- | --- |
| cmake -S . -B build; cmake --build build -j4; ctest --test-dir build --output-on-failure | 33/33 PASS |
| -DMADOBS_BUILD_RUNTIME=OFF; build; ctest | 2/2 PASS |
| cmake --install build-api; new C-only consumer find_package(MadObs), MadObs::api | PASS; executable returns 0 |
| cc -std=c11 -Wall -Wextra -Werror samples/api_only/main.c -Iinclude | PASS; executable returns 0 |
| -DIMOBS_SAN=asan default LeakSanitizer | BLOCKED: LeakSanitizer cannot run under environment ptrace |
| -DIMOBS_SAN=asan; ASAN_OPTIONS=detect_leaks=0 ctest | 32/32 PASS for AddressSanitizer + UBSan; NO leak validation claim |
| -DIMOBS_SAN=tsan; build; ctest | 32/32 PASS for ThreadSanitizer |
| git diff --check | PASS |
| Narrow private-key/common token format scan | 0 matches; not exhaustive security review |

Sanitizer configurations omit the shared-symbol audit by the retained build design;
that audit passed in the normal 33-test configuration. Original unmodified tests
remain except explicit callback timing, control-key opt-in fixtures and portable
crash fixture. No tests deleted to make failures green.

Six new safety regression cases include cross-thread callback deferral, callback
following Healer destruction, missing secret defaults, cleanup while disabled,
completion queue saturation counts, forged envelopes using missing/development key.
The new regression executable crashed against the original implementation when a
completion ran after destruction (captured in regression-before.log), and all six
cases now pass, including sanitizer runs.

## Scope not complete / release gates

- O01 remains PARTIAL: new generic observer API has no full generic-schema runtime
  bridge yet; existing legacy native API remains the runtime integration path.
- New Mad-prefixed runtime C ABI, generic trace/span schema, observer unregister/drain,
  separate opt-in healing binary, platform bindings and five SDK adapters not implemented.
- Legacy raw logs are preserved, NOT automatically scrubbed for privacy; a generic
  redaction engine and privacy contract tests remain.
- LICENSE absent upstream; owner license/ownership review required before public release.
- Leak detection blocked by current environment, not proven clean.
- BoringSSL real provider not present/tested; no AES/Ed25519 capability claim for this build.
- iOS/Android/OHOS cross-build, device tests, resource/perf budgets, packaging/signing,
  exhaustive secret/dependency review, SBOM and long-running stress remain unverified.
- Host must stop/join producers before observer context destruction; concurrent
  unregister/destroy is not supported by the draft observer contract.
- C host transport/healing done callbacks must execute exactly once; omitted callbacks
  may retain their bridge context. Full timeout-owned callback handle API remains future work.

## Rollback

No external state changed. Source baseline commit above is immutable locally. Review
`git diff 461b6797..` or create a separate worktree from that commit; do not discard
later work. Disk/wire formats unchanged; remote target remains untouched.
