# Tenth increment: opt-in host-JVM private binding

Status: tested on Linux x86_64, GCC 14.2 and official Eclipse Temurin 21.0.13+9.
This adds an actual JNI shared library and Java AutoCloseable facade, not an
Android AAR/NDK/device qualification. Existing native-only/API-only defaults remain
independent of Java. See bindings/jvm/README.md for complete lifecycle semantics.

## Implementation and provenance

- Explicit MADOBS_BUILD_JNI=ON; runtime must be enabled. Optional target makes the
  native static runtime PIC and links it privately into libmadobs_jni.so. Linux
  export version script restricts the final surface to six JNI entry points.
- javac generates the native signature header; C++ includes it, so Java/native
  signature drift fails compilation. JAR contains only the facade and Exporter,
  not test classes. BUILD_TESTING=OFF builds JNI/JAR without compiling tests.
- Private policy native copying, bounded numeric event input, C registry generation
  tokens, host-owned WAL directory and fail-closed replay reuse the native API.
- Java monitor serializes close; emit can race close safely. Callback-side close
  is rejected before taking the monitor. Exceptions become counted NACKs, not
  unhandled worker Java exceptions. Byte arrays are owned Java copies.
- Explicit close mandatory. No finalizer/cleaner: abandoned owners retain resources
  until process exit. Process-lifetime registry avoids calling a destroyed JVM
  from static destructors. It is not a guarantee against leaks by incorrect hosts.
- Development JDK downloaded from the official Adoptium release URL and checked
  against official API-reported SHA256. Exact URL/version/hash in
  deps/temurin-host-jdk.lock.json. No JDK binary or generated header is vendored.
  This pins bytes, not a general supply-chain attestation or security approval.

## Verification

- JNI-enabled Release suite 44/44, including real JVM -Xcheck:jni and synthetic
  JNI lookup-failure injection. Final log: verification/phase10-final.log.
- JVM suite repeated 20 times after drain-wait coverage was added: all passed.
- Actual installed/relocated JAR+so consumer compiled separately against the JAR,
  then ran the full Java smoke with -Xcheck:jni. Export list and JAR contents in
  verification/phase10-installed-final.log. This relocation test does not hide
  original source/build paths in a mount namespace.
- Fresh scripts/verify-jvm.sh run includes build/full CTest/install/relocation,
  separate consumer, exact JNI export audit and negative runtime-OFF configure.
  Expected negative configuration failure is logged, not counted as test failure.
- BUILD_TESTING=OFF JNI+JAR builds and reports Total Tests: 0; no JvmSmoke.class
  produced. Log: verification/phase10-tests-off.log.
- Existing standalone matrices rechecked: native 42/42, Release 42/42, real
  BoringSSL 43/43, API-only 6/6, ASan+UBSan 41/41 (leaks disabled), TSan 41/41.
  These native sanitizer runs are NOT JVM/native sanitizer interoperability tests.
- Independent review's GetObjectClass-null concern fixed with checked method
  resolution. Synthetic function-table injection proves short-circuit/no extra
  method call and original exception preservation for class/method lookup failure.

## Gates not passed

No comprehensive JVM allocation/attachment failure injection: GetObjectClass and
GetMethodID failure paths are synthetic; AttachCurrentThread/NewGlobalRef/
NewByteArray OOM are not all injected. No classloader hot-unload or abandoned-owner
shutdown guarantee, blocking/async exporter support, Android GC/NDK/AAR, Xcode/
Swift/XCFramework, OHOS/HAR, device or physical-power-loss test. Optional BoringSSL
plus JNI is not in this stage's tested package configuration. The full source
publication and host-JVM CI are separate from a release or platform qualification.
