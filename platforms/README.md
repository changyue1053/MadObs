# Native mobile integration (experimental)

The same installed `madobs/observer.h`, `madobs/privacy.h` and `madobs/runtime.h`
C ABI is the native interface on Android, iOS and OHOS. The first two headers are
runtime-free and require neither another SDK nor a global observer registration.
Use `madobs_noop_observer()` by default. Host injection and registration are opt-in.

`../scripts/build-platform.sh android|ios|ohos` configures the official installed
SDK toolchain, disables desktop-only tests/tools and builds a static archive.
No SDK downloads, upload, signing, publication or credentials are performed.
Set `CMAKE` if CMake is not on PATH. Set `MADOBS_BUILD_DIR` and `MADOBS_INSTALL_DIR`
for isolated output. External ZLIB must resolve for the target, not host architecture.
An optional real BoringSSL provider is not bundled; null provider is not encryption.

| Target | Environment required | Default native target | Verification here |
|---|---|---|---|
| Android | `ANDROID_NDK_HOME` official NDK | arm64-v8a, API 24 | Missing NDK; not built |
| iOS | macOS/Xcode, `xcrun` and iphoneos SDK | arm64, iOS 13 | Linux; not built |
| OHOS | `OHOS_NATIVE_HOME` native SDK | arm64-v8a | Missing SDK; not built |

Overrides: `ANDROID_ABI`, `ANDROID_PLATFORM`, `IOS_DEPLOYMENT_TARGET`, `OHOS_ARCH`.
Defaults are proposed build inputs, NOT validated support minima. iOS simulator,
Android other ABIs and OHOS other architectures require separate build directories.
The script does not yet produce a multiarchitecture XCFramework, AAR or HAR.
Kotlin/JNI, Swift ownership facade, and ArkTS/N-API facade remain pending target-SDK
validation. Do not cast Java/ArkTS numeric handles through floating-point numbers;
future managed facades must retain native handles losslessly and enforce ownership.

## Native host contract

- Create runtime with an application-private writable WAL directory and a native
  nonblocking exporter callback; `done` must be called exactly once per batch.
- Configure/register before start. Runtime and observer binding have explicit owners.
- Runtime handle is not a pointer; do not synthesize it. Keep callback context and
  host-owned observer binding alive and immutable for all producer calls.
- Emit only finite-domain numeric event schemas. Use `madobs_observer_emit_private`
  for the policy-controlled path. It strips exact timing and unregistered attributes.
- Stop/join producers before runtime stop/destroy. Stop is terminal, not restartable.
- Callback dispatch is not persistence or remote acknowledgement. Inspect counters.
- Opt-in crash interception/logs/healing remain separate explicit calls; no automatic
  process-wide crash installation or raw logging is added by the generic interface.

`find_package(MadObs)` then link `MadObs::runtime` for native runtime; link
`MadObs::api` for the header-only SDK contract. See independent C consumers in
`samples/`. Desktop native builds/consumers are tested; they do not prove mobile
ABI, lifecycle, packaging, store compliance, power handling or device performance.

## Owning C binding for platform adapters

`madobs/private.h` now provides a real exported owning C ABI backed by the tested
PrivateRuntime. `madobs_private_open/emit/stop/destroy/get_stats/get_gate_stats` use
an independent generation-tagged uint64 handle registry. Copying a numeric handle
does not create an owner; destroy revokes it, while already-acquired native operations
retain safe shared ownership. Stop preserves the handle for final stats; destroy
removes it. Reopen creates a different generation, never a recycled live identity.

| Adapter | Required representation | Status here |
|---|---|---|
| JNI/Kotlin/Java | Preserve uint64 bit pattern in jlong; native methods validate/marshal structs | C ABI tested; JNI not compiled |
| Swift | Import C header, UInt64 token, retain exporter context until explicit stop/destroy | C ABI tested; Swift not compiled |
| OHOS ArkTS/N-API | BigInt or opaque native object, never JavaScript Number for handles | C ABI tested; ArkTS/N-API not compiled |

No adapter should reproduce struct offsets manually. Use target compiler C headers,
copy policy arrays for the duration of open, and keep exporter context alive until
stop/destroy finishes. Both lifecycle calls are forbidden from exporter callbacks.
C entrypoints contain C++ exceptions; invalid pointers still remain caller errors,
not recoverable exceptions. Successful dispatch/admission is not remote delivery.

`samples/private_c` is a C-source independent installed-package consumer tested in
Release. CMake enables both C and C++ because the native implementation is C++.
This evidence does not substitute for actual JVM/Swift/ArkTS marshaling, platform
linkage, crash-safe mobile lifecycle or packaged binaries.

### C lifetime completion and error scope

Legacy C Open returns 0 on failure; the additive OpenEx reports coarse reason
codes for configuration, WAL identity/access/lock and native startup failures. Do not treat 0 as a reason
to modify/delete WAL metadata. C destroy removes the token before waiting for drain:
a concurrent second destroy may return INVALID_HANDLE while the first is still
working. INVALID_HANDLE is NOT a drain-completion signal. Use one host lifecycle
owner to stop, read final stats, destroy and then release callback/context storage;
other threads must not free that context on an invalid-token result.
