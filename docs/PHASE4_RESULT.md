# Fourth local increment: owning privacy runtime binding

Status: native ownership/privacy substrate implemented; release remains BLOCKED by
actual platform/packaging resources and privacy recovery design. No push/publication.

- Added optional C++11 PrivateRuntime: validated deep-copied immutable policy,
  private-only Emit, shared ownership, admission-vs-close synchronization, final stats.
- Concurrent Close callers wait for one drain/destroy; no lock around exporter callback.
- Independent review found previous raw/broader-policy WAL could replay without current
  filtering. Fixed conservatively by atomic new-directory-only Open (0700), rejecting
  ANY existing WAL. No claim of privacy-safe restart support; this is now an explicit
  release gate, not silently discarding or exporting historical data.
- Five tests: stack policy lifetime/minimization; 20x4-thread Emit/Close race with exact
  accepted=drained counts; raw-WAL and tighter-policy reopen rejection; eight simultaneous
  Close callers observing identical final drain statistics; symlink/dangling symlink
  rejection and eight contenders for the same new directory (exactly one owner).
- Independent re-review also confirmed replay refusal and path race rejection. When
  native resource slots are exhausted Open can leave its new directory; host must
  clean it safely or choose a fresh path. No automatic deletion is attempted.
- Exploratory export-count equality was intentionally replaced with accepted/drained
  equality: Stop is not a delivery acknowledgement and may leave WAL records unexported.
  The raw runtime behavior was not changed or misrepresented as guaranteed delivery.

## Verification

Native CTest 39/39, ASan+UBSan 38/38 (LSan disabled for existing ptrace limitation),
TSan 38/38 and API-only 6/6 passed. Fresh static-only installed runtime consumed from strict C++11
Release sample, exit 0. Existing raw ABI/bridge/privacy tests remain passing.
Evidence in docs/verification/phase4-*.log; git diff --check passed.

## Substantive blockers

- Android: only JRE present, no javac/jni.h/NDK. iOS: no macOS/Xcode SDK. OHOS: no native
  SDK or ArkTS toolchain. Managed bindings/platform packages/device tests cannot be
  verified here. Native C++ substrate is NOT an AAR/XCFramework/HAR or JNI/Swift/ArkTS.
- PrivateRuntime replay requires reviewed policy-bound persistence and migration design;
  current new-directory-only restriction safely prevents replay, not production recovery.
- Existing source lacks license; ownership/public license choice cannot be inferred.
- Real provider/encryption, platform device performance, LSan, physical failure tests,
  SBOM/security audit, schema inventory and ABI freeze remain prior release gates.

See docs/PRIVATE_RUNTIME.md for lifecycle and privacy contracts. This wrapper does
not promise lock-free latency or close from exporter callback; those are unsupported.
