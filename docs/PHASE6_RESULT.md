# Sixth local increment: owning C binding and blocked-batch diagnostics

Status: locally verified native/foreign-interface substrate; release/platform gates
remain incomplete. No push or publication.

## Implemented

- Six real exported owning C entrypoints in madobs/private.h: open, emit, stop,
  destroy, native stats, gate stats. Independent monotonically increasing tagged
  uint64 handles, bounded registry, shared acquisition before lifecycle calls.
  Destroy revokes the token before closing; in-flight acquired calls stay alive.
- Exceptions contained at C boundary; policy copied by underlying owning runtime.
  Stale/invalid handles and undersized output buffers reject. Callers retain responsibility
  for valid memory, callback/context lifetime and no close inside exporter callbacks.
- Independent review found bool codec helpers conflated zlib allocation failure with
  malformed/noncanonical bytes. Added detailed internal codec status while retaining
  the old bool interfaces; linker fault injection checks compress2/uncompress
  Z_MEM_ERROR -> RESOURCE_FAILURE, malformed input -> BAD_WIRE and retry success.
- Additive madobs_check_private_batch reason enum; existing boolean validator retained.
  Gate snapshot contains only numeric rejection attempts by priority, last batch id
  and reason. No record, string/path, or payload bytes are exposed. Sticky attempt
  history is not a persistent inventory or proof the batch is still queued.
- Safe recovery runbook: inspect, stop producers/runtime, preserve old WAL, resume
  future collection in a fresh private directory. No automatic deletion, force ACK,
  policy relaxation, raw fallback, historical repair or migration is implemented.
- Regression proves blocked contaminated WAL never exports, fresh replacement exports
  valid future events, and old WAL remains blocked when reopened. C lifetime stress
  tests race emit/destroy without dangling pointers; stale generation and C consumer
  contracts are covered. Open 0 has no detailed reason. Concurrent second C destroy
  returning INVALID_HANDLE is explicitly NOT proof another caller has drained;
  lifecycle completion/context release belongs to one coordinated host owner.

## Evidence

Final matrix: native 41/41, ASan+UBSan 40/40, TSan 40/40, API-only 6/6 PASS.
Fresh installed strict C-source and strict C++11 Release consumers both exit 0.
Evidence: docs/verification/phase6-*.log. Independent review verified both zlib fault
classifications after correction; lifecycle/lock-completion caveats are in the public
C header and operational runbook.
ASan uses detect_leaks=0 due existing ptrace/LSan restriction. Host is Linux x86_64.
Independent C-source Release consumer and strict C++11 consumer exercise installed
headers and archive, not in-tree include shortcuts. Original raw ABI/Noop/API-only
contract remains unchanged.

Tool audit: javac/JNI headers absent (JRE only), xcrun/swiftc absent, Android NDK and
OHOS native SDK environment roots unset. No actual JNI, Swift or ArkTS wrapper compile,
AAR/XCFramework/HAR package, device testing or signing is claimed. Platform mapping guide
explains handle width and ownership, not a fictitious verified managed-language facade.

## Remaining operational/security gates

Policy/codec migration and historical blocked-WAL repair remain explicit offline work.
A rejected head may stall one priority; underlying P1/P2 retention may overwrite data,
so no indefinite quarantine or lossless guarantee is claimed. Batch/network timing
remains visible. New numeric diagnostics should still obey host logging policy.

Source license/ownership, real crypto provider, LSan, physical power/ENOSPC tests,
platform budgets, packaging, SBOM/security review and stable public ABI remain gates.
