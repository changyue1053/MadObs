# Seventh local increment: safe open diagnostics and versioned package checks

Status: locally verified native contract/distribution increment, not platform release.
No push/publication. Existing public APIs remain callable with unchanged signatures.

## Added

- `madobs_private_open_ex` and optional C++ diagnostic output. Codes distinguish invalid
  arguments/policy, unavailable/unsafe/unbound storage, policy-or-manifest mismatch,
  lock busy, IO, native create/start, binding capacity and internal failure. No path,
  event or raw metadata is exposed. Mismatch deliberately does not invent corruption
  vs policy-change certainty. Legacy Open still returns zero on failure.
- Identity file size refreshed after obtaining the lock to avoid reading the pre-lock
  initializer snapshot. Fstat IO failure is distinguished from permission/type rejection.
- C contract cases for lock busy, invalid argument/policy, unbound directory, altered
  policy and unsafe permissions; all preserve fail-closed behavior without mutations
  to existing data or automatic repairs.
- Equal-requested-version CMake metadata for experimental 0.1.0, with architecture-
  independent API-only package metadata. Unversioned find_package remains supported;
  metadata does not force pinning or the EXACT keyword. No stable major ABI promise.
- Offline verify-installed.sh: fresh API-only installation, C consumer, fresh native
  static installation relocated to a different prefix, C and C++11 consumers, and
  rejection of incompatible exact requested package version. No SDK download/signing.

## Evidence and remaining gates

Final matrix: native 41/41, ASan+UBSan 40/40, TSan 40/40, full Release 41/41,
API-only 6/6 PASS. Fresh offline API/native installs and relocated C/C++11 consumers
pass; the original unversioned runtime consumer passes and requested 9.9.9 rejects.
Full matrix and fresh package run are in docs/verification/phase7-*.log. ASan leak
checking remains disabled under the known ptrace limitation. Distribution evidence is
Linux native/API-only, not a real AAR/XCFramework/HAR or managed marshaling test.

Open diagnostics are snapshots, not authorization to delete/relax/rewrite storage.
A first initializer can briefly yield UNBOUND to another contender; bounded same-policy
retry is safe. Manifest mismatch, policy migration and blocked historical recovery need
host review. Parent directories must remain private and externally unmodified.

Actual mobile SDK/JDK JNI headers/Swift/OHOS toolchains remain unavailable here. Source
licensing, device tests, real crypto provider, LSan, physical power/disk failure tests,
SBOM/security assessment and ABI/release approval remain substantive release blockers.

## Release-only build and optional provider limits

The first fresh Release install run failed with GCC14 -O3 -Werror=free-nonheap-object
in retained log-store and policy-envelope reserve/push/insert constructions. Saved before-fix evidence
is phase7-package-before-fix.log and phase7-package-policy-before-fix.log. Construction now resizes once and copies into known
offsets, preserving format bytes without disabling the warning. Final results are
recorded separately; prior RelWithDebInfo builds were not proof of Release packaging.

Relocation evidence covers the default null-provider native static configuration ONLY.
An explicitly supplied BoringSSL archive remains a PUBLIC absolute link dependency in
installed targets. The configure-only synthetic-path export audit demonstrates this
blocker but does not compile/test BoringSSL or its cryptography. That configuration is
NOT certified relocatable or distributable; a verified provider packaging contract and
real provider tests are required. No source license or third-party redistribution right
is assumed from the ability to generate an archive.

## Failure injection scope

Added linker-injected ENOSPC for manifest write and fsync. Failed partial writes yield
IO_ERROR and subsequent partial-identity mismatch; failed fsync yields IO_ERROR even
on an existing matching identity until syncing succeeds. Recovery now re-syncs matching
manifest/directory before starting WAL, so a prior cached-but-unsynced identity cannot
skip the durability precondition. These tests do not fill a physical filesystem or
simulate machine power loss. Open allocation-OOM injection remains uncovered; codec
allocation failures have separate phase-6 injection coverage.
