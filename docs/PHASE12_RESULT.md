# Twelfth increment: verified PIC provider plus host-JVM binding

Status: one Linux x86_64 / GCC 14.2 / Temurin 21.0.13+9 combination passes.
Production native/JNI code is unchanged; this increment adds a combined build,
installation evidence and CI coverage. Not Android or another mobile platform.

## Fixed inputs

- MadObs native/JNI source: local 117d86fe443482b57ef67c752706eee809e34a73,
  tree-identical to public 7416b17a21683715c72b184f76217918fab2aa50.
- Official BoringSSL source 533902fb2e8fd0586db7d58879d697721726605a, source
  archive and full-tree checks as in deps/boringssl.lock.json / Phase8.
- Same locally built Release PIC libcrypto SHA256
  8a715430ae5ca6f471206132ebbc69f54d42d7f495ec05beb6e028ca875a78c0.
  Binary hash identifies bytes; it is not a signed reproducible-build attestation.
- Official host JDK pinned by deps/temurin-host-jdk.lock.json. No JDK binary is
  redistributed. New fetch-host-jdk.sh validates SHA256 before safe extraction,
  rejects existing destinations and supports only Linux x86_64.

## Results

- New bootstrap script actually downloaded/verified the official pinned JDK.
  Mocked corrupt download fails checksum before extraction; existing destination
  is rejected. Evidence: verification/phase12-jdk-fetch.log.

- MADOBS_BUILD_JNI=ON + real provider + MADOBS_PACKAGE_BORINGSSL=ON Release build:
  46/46 CTest passed, including provider-required native contracts, synthetic JNI
  failure boundaries and real JVM -Xcheck:jni tests.
- Verified-provider package installs its crypto archive, provenance and unchanged
  upstream/Fiat license files alongside the JNI library and JAR.
- Separately compiled Java consumer against the installed JAR, then ran full JVM
  smoke against a copied installation. Source/build paths were not hidden in a
  mount namespace for this Java consumer test.
- The resulting JNI library contains the provider implementation (local
  EVP_AEAD_CTX_new symbol observed), but exports only six JNI entry points.
  The archived crypto bytes in the copied installation match the fixed hash.
- Evidence: verification/phase12-provider-jvm.log and
  verification/phase12-provider-jvm-installed.log.
- CI provider job now builds/tests this combined configuration and the installed
  native encrypted-log and Java consumers. Independent default/native and
  null-provider host-JVM jobs remain. No cross-SDK default dependency was added.

## Limits

This proves neither arbitrary mixed-provider C++/shared/static compatibility nor
FIPS/security approval, whole-JVM OOM safety or device behavior. Bare static MadObs
and BoringSSL archives still have generic upstream symbols; the JNI library's
restricted exports do not isolate every possible host configuration. No Android
NDK license was accepted or NDK downloaded. Android, Swift/Xcode and OHOS package
and device gates remain. Explicit close / nonblocking exporter contracts remain.
