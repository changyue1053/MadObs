# MadObs second local increment

Status: native generic bridge + Mad ABI implemented and verified; full SDK release
remains PARTIAL. This supersedes phase-1 statements that those two features are absent.
No remote mutation, push or publication. Source baseline and licensing gate unchanged.

## Delivered

- 30 real exported Mad-prefixed C symbols; existing imobs_ ABI retained.
- Generic observer event admission into a 1024-slot fixed-size queue with bounded
  producer retries, no producer IO/allocation/mutex; invalid input rejected.
- Existing consumer thread serializes G1 records to existing priority/WAL/exporter.
  All metadata and eight attributes survive a field-by-field end-to-end test.
- Decode API with version/size/overflow validation, truncation and random-input tests.
- Failed-export WAL survives runtime destruction/recreation. This is NOT a power-loss test.
- MadObs::runtime install/export package with a fresh C-source consumer; no other SDK.
- API-only consumer remains standalone, including -fno-exceptions C++ compilation.

## Independent review findings addressed

1. Healer was implicitly copyable with a shared completion inbox. Copy/move are now
   explicitly deleted; compile-time traits regressions enforce single ownership.
2. observer.h unconditional try/catch broke no-exception consumers. Compiler exception
   feature macros now choose the appropriate path; an actual -fno-exceptions target runs.
3. Generic WAL loss originally used Submit's buffer-admission return value, producing
   false zero counts. Pipeline now tracks generic record attribution inside each batch
   and counts actual append rejection; counters update after delayed flush too.
   Reviewer reproduction: 100 P0 events, WAL 512x2 -> 84 reported generic rejections,
   matching 84 rejected batches. New P1 test: 100 buffered records in one oversized
   batch -> 100 rejected records, NOT 1 batch; before flush remains 0.
4. start after stop previously returned OK although worker stayed stopped. stop is now
   explicitly terminal and restart returns BAD_STATE; running start remains idempotent.
5. emit/stop concurrency was outside the lifetime design but ambiguously documented.
   Contract now explicitly requires stopping/joining producers before stop/destroy;
   no concurrent admission/drain guarantee is claimed. EMITTED means callback invocation
   only, not enqueue/durability/delivery. This is a scoped contract, not a race-proof API.

Added real worker-completion overlap with Tick/destruction over 100 iterations under
sanitizers. No synchronous-only test is offered as race coverage.

## Verification

- Native CTest: 35/35 PASS, including shared-symbol/hotpath/signal-safe audits.
- ASan+UBSan: 34/34 PASS with ASAN_OPTIONS=detect_leaks=0.
- TSan: 34/34 PASS.
- API-only: 3/3 PASS, includes C, C++ exception firewall, C++ -fno-exceptions.
- Installed runtime consumer: find_package(MadObs), link MadObs::runtime, compile C
  source with C/C++ toolchains, emit/stop/stats/destroy executable returns zero.
- Seven safety regression cases and six generic-bridge cases are included.
- No LeakSanitizer proof: current environment ptrace restriction persists.
- WAL-capacity rejection tested; physical filesystem ENOSPC/power failure NOT tested.

Commands: cmake -S . -B build; cmake --build build -j4; ctest --test-dir build.
Repeat with -DIMOBS_SAN=asan or tsan; API-only -DMADOBS_BUILD_RUNTIME=OFF.
Logs are in docs/verification/phase2-*.log. Sample consumer is samples/runtime_c.

## Remaining boundaries

ABI is experimental, not frozen. Binding memory is host-owned; no concurrent unregister
or runtime destruction. Stats are independent atomic snapshots; wal_dropped counts
append rejection only, not old-segment eviction or all historical losses. Host exporter
must be nonblocking and call done exactly once; stop flushes local buffers and attempts
export but does not guarantee remote acknowledgement. New G1 events require a compatible
Host receiver; the legacy thin-server query model was not expanded into a backend project.

Generic schema registry/redaction policies, generic span semantics, separate healing
package, platform bindings/device QA, real BoringSSL-provider test, LSan, performance
budgets, exhaustive security/supply-chain review and public license remain release gates.
