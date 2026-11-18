# Fifth local increment: policy-bound fail-closed recovery

Status: native private-runtime recovery implemented and locally tested; mobile/release
certification still incomplete. No remote mutation or publication.

## Design and changes

- Exact versioned policy identity, fixed LE fields only, no padding/hash ambiguity.
  Policy tightening, relaxation, version/ordering changes reject storage; no migration.
- Owned 0700 directory, owned single-link 0600 regular manifest, no-follow opens,
  write+fsync identity before runtime, advisory exclusive lock until native destruction.
- Old raw/missing/corrupt manifest rejected. Raw corruption of an already-bound directory
  cannot bypass policy: exporter validates every record and full canonical envelope.
- Added actual C ABI `madobs_validate_private_batch`; no existing ABI removed. Private
  runtime uses it before forwarding any bytes. Unknown records, extra values/timing,
  malformed/noncanonical encodings, compressed ignored suffixes or bad batch IDs reject
  the whole batch. Rejected batches remain unacknowledged, with attempted-rejection count.
- Complete canonical envelope comparison includes recompression. This is deliberately
  fail-closed, not a cross-zlib-version promise. Batch/network timing remains visible.

## Verification scope

Private binding cases include stack-policy lifetime, concurrent Emit/Close and concurrent
Close; path links and eight competing openers; fork/_exit without destructor followed by
same-policy recovery; independent exec process lock contention; tighter/looser policy
rejection; corrupted identity rejection; raw contamination inside a bound directory.
Wire tests include plaintext/compressed canonical batches, every truncation length, bad
batch ID and an appended suffix ignored by ordinary zlib decoding but rejected here.

Final matrix: native 39/39, ASan+UBSan 38/38, TSan 38/38, API-only 6/6 PASS.
Fresh static-only installed strict C++11 Release consumer opens/emits/closes/reopens
same-policy storage successfully (exit 0). Native suites contain seven PrivateRuntime
cases and eight bridge cases. Independent review reproduced original raw replay denial,
confirmed no new blocker in this increment, and verified same-policy retry after resource
failure. Evidence is recorded in docs/verification/phase5-*.log.
ASan leak detection remains disabled because of the pre-existing ptrace limitation.
Actual physical power failure/ENOSPC, target mobile builds, signing and devices remain
unverified. Process-crash testing is not represented as storage power-loss certification.

## Remaining boundaries

Parent directories must be private and unmodified by external writers. Advisory locks
cannot coordinate arbitrary raw users; byte-level export policy gating is the second
boundary. No protection against privileged/same-uid hostile tampering is claimed.
Initialization failures may leave metadata for host inspection; no automatic deletion.
Rejected WAL can block a priority indefinitely and requires host intervention. No unsafe
raw replay or implicit discard path exists. Policy/codec migrations remain explicit work.

SDKs/managed toolchains for Android/iOS/OHOS, source public licensing, actual crypto
provider, LSan, platform/performance budgets, SBOM and release ABI review remain gates.
The native C++ binding and build scripts are not validated JNI/Swift/ArkTS packages.
