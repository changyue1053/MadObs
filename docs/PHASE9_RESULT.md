# Ninth increment: fixed host cross-SDK composition

Status: Linux x86_64 GCC 14.2 composition fixture passed. Not a mobile SDK release,
network interoperability test, or general static-provider compatibility claim.
Stage8 public commit b2428afc02b4ffda1ae1c745ff82862d7671b101 CI completed successfully:
https://github.com/changyue1053/MadObs/actions/runs/44284263921

## Exact inputs and result

- MadObs package from Stage8 local 2c14489de3ba1cd86ae2b4d07263e69e784aef99
  (tree-identical to the public commit above), Release, PIC real crypto provider.
- MadLink frozen header commit 53668ab8a06ac09d07dcadefccbcf106b35dff79;
  headers independently compared byte-for-byte with that git commit, listed in
  verification/phase9-madlink-headers.json. Peer-provided stable Release libraries,
  BUILD_TESTING=OFF; archive hashes below identify actual inputs, not a signed
  provenance/reproducible-build proof.
- Single BoringSSL crypto provider, upstream commit
  533902fb2e8fd0586db7d58879d697721726605a. SSL archive from the same source revision;
  no second crypto archive and no whole-archive flags. Existing upstream symbol
  names remain unisolated.
- Existing native suite rechecked 42/42 and real-provider suite 43/43; logs
  in verification/phase9-native.log and phase9-boringssl.log.
- 1/1 CTest (also repeated 20 times without failure) passed with strict -Wall -Wextra -Werror; log in
  verification/phase9-composition.log. Instantiates and destroys LinkClient,
  waits for pending releases, exercises adapter filtering, callback dispatch vs
  private admission/rejection and private runtime export. No network Start,
  handshake, reconnect, QUIC or request traffic exercised.
- The private runtime export gate enforces current policy before callback. This
  fixture checks an export occurred; detailed wire/timestamp rejection remains
  covered by existing private-gate tests, not a new wire decoder here.

SHA256 actual archives:

| Archive | SHA256 |
|---|---|
| MadLink libimlink.a | 5c0d253536790a57d44e2dfc2cf929ca938133f0b144133596cb0570d9c2993f |
| MadLink libimlink_core.a | b79123693602f3bc58545691c1ef422b3fdebedb8a92f875f5744c7f8162fbd1 |
| BoringSSL libssl.a | 79aa477a6ab7986ecfa126049cefb09d102369a13f2a4b4bc88f2f7b8a64414d |
| MadObs packaged libcrypto | 8a715430ae5ca6f471206132ebbc69f54d42d7f495ec05beb6e028ca875a78c0 |
| MadObs libmadobs_runtime.a | bea81b6d5ecd4eaffbd91bb054c1ff8f9ebbe6d5f91540170760064fb1f0a358 |

## Limits / remaining work

This proves one host configuration with externally supplied frozen artifacts.
It is not wired into default CI because that would make another SDK a required
build input; MadObs remains independently buildable. MadLink currently has no
complete installed CMake package. Arbitrary mixed versions, multiple providers,
whole-archive, shared/static interposition and cross-compiler C++ ABI combinations
remain unverified. No SDK identifier registry is frozen.

This environment has a Java runtime but no javac/JNI development headers, no
Android NDK, no Xcode/Swift compiler, and no OHOS SDK. No JNI/AAR, XCFramework,
NAPI/HAR or device execution was performed in this increment. These are real
platform-toolchain gates, not passed by the Linux composition smoke. Existing
platform build scripts remain entry points, not produced distribution artifacts.
