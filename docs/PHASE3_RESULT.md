# Third local increment: explicit privacy helper and mobile native build preparation

Status: local deliverables below VERIFIED; overall SDK release remains PARTIAL.
No push/publication. The original IMObs source baseline, ownership/license gate,
compatibility ABI and wire tags remain unchanged.

## Delivered

- Header-only `madobs/privacy.h`: host-owned finite-domain immutable policy, at most
  64 events × 8 keys × 16 enum values, full policy validation and duplicate rejection.
  Unknown event/version, malformed input and excess severity reject; unregistered
  keys/out-of-domain values drop; exact monotonic time and inactive slots are zeroed.
  Empty policy denies all; default Noop needs no policy. Output untouched on failure.
- Explicit helper, NOT a mandatory runtime interceptor. Raw emit/legacy logging keep
  their prior behavior. No claim of automatic PII detection, consent or anonymization.
  Reviewed per-SDK schemas, actual transport timing privacy and rate limiting remain.
- C, strict C++11 and strict no-exceptions C++ consumption tests. Independent review
  found C++ `{0}` warning under -Werror; fixed with C++ `{}` and tests that reproduce it.
- Privacy -> runtime -> failed exporter -> WAL -> recreated runtime -> exporter replay
  test proves only registered finite-domain fields reach exported event records;
  rejected event is not queued, precision timing and other seven slots are cleared.
- Android/iOS/OHOS native static build script and native C ownership/interface guide.
  Script syntax and missing-SDK failures verified; no platform build or device claim.
- BUILD_TESTING=OFF now really omits legacy tests/tools; MADOBS_BUILD_SHARED=OFF
  supports static-only native archive preparation. Default legacy shared build kept.
- Fresh installed API-only C and runtime C-source Release consumers compile and run.

## Final verification

- Native: 38/38 CTest PASS, including symbol/hotpath/signal audits and seven bridge cases.
- ASan+UBSan: 37/37 PASS with ASAN_OPTIONS=detect_leaks=0. LSan still blocked by ptrace.
- TSan: 37/37 PASS.
- API-only: 6/6 PASS, including strict privacy C++ and no-exceptions privacy consumers.
- Fresh BUILD_TESTING=OFF / static-only build: 0 tests registered; archive installs.
- Installed runtime consumer (raw + privacy event): exit 0; installed pure C API-only
  consumer: exit 0 without native runtime dependency.
- bash syntax PASS. Missing Android NDK, macOS/Xcode, OHOS native SDK fail early and
  explicitly, as recorded in verification/phase3-platform-blockers.log.
- git diff --check PASS.

Evidence: docs/verification/phase3-*.log. Builds use GCC 14.2.0, CMake 4.4.5,
Linux x86_64 and system zlib. This is NOT mobile cross-compilation evidence.

## Still not complete

Android NDK/JNI/AAR, Apple Swift/XCFramework/signing and OHOS ArkTS/N-API/HAR require
real SDK/platform build validation. Current mobile deliverable is a native C ABI
integration guide and static-build entrypoint only, not managed-language facades.
Licensing, reviewed SDK event inventories, optional separately packaged healing,
BoringSSL provider checks, LSan, physical ENOSPC/power-loss tests, performance gates,
SBOM, release packaging and public ABI freeze remain gates from phase 2.
