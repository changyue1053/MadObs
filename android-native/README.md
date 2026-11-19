# Android native installed C SDK preview

Source-only build/link checkpoint from public MadObs
4af77ffa464a48307e5ff2d95f10b835919d205b (tree
3a03801beb18c9601630cc4421b95c2743018dfb). No license or redistribution permission
is added. No upstream source is vendored, no artifact is uploaded/published, and
existing runtime, Java/JNI, host checkers, licenses and workflows are unchanged.

The opt-in workflow runs on pushes only to checkpoint/madobs-android-native-preview
(and optionally manual dispatch once discoverable). Publication/run authorization
is separate. Checkout explicitly selects and verifies the exact event SHA.
It uses Ubuntu24.04, official NDK r28c 28.2.13676358, SDK CMake3.22.1, Android API24
and ARM64. Official package provisioning uses existing runner licenses and closed
stdin, not license-acceptance piping. The SDK manager runs on the existing runner;
this is not a Java/JNI SDK test, and no Java source/checker is changed or executed.
The checkout action is commit-pinned, credentials are not persisted, and permissions
are read-only. No remote service, device, emulator, signing, APK, or release step exists.

A disposable producer builds only static imobs_core with JNI, shared libraries,
all upstream tests and BoringSSL packaging disabled. Provider inputs are empty:
null crypto, not encryption validation. Both producer and consumer explicitly use
the NDK target sysroot zlib header and API24 ARM64 libz.so, never host zlib.
MadObs's exported runtime target resolves ZLIB::ZLIB again in the consumer.

The installed prefix is moved to a path containing a space; disposable producer
source/build is deleted before independent C consumer configuration. Only installed
public headers and the exported MadObs::runtime target are used by that consumer.
Package registries are disabled and exported CMake files are checked for deleted
producer paths. Consumer C code links through C++ linkage tooling with static libc++.
The .a has archive-magic validation; 16 KiB is NOT a property asserted for .a object
files. The separately linked ARM64 PIE is checked for ELF architecture/type and all
LOAD segment alignment/congruence >=16 KiB, and exactly one PT_INTERP naming
/system/bin/linker64 (distinguishing the dynamic executable from a shared object). This is not APK ZIP alignment evidence.

The synthetic native C executable contains local sink/start/emit/stop/counter checks
but is NOT executed by this checkpoint. It accepts only an explicit local WAL path,
contains no endpoint, and stops its sole producer before runtime shutdown. Its
source assertions are not Android runtime evidence. It does not prove privacy,
persistence, real telemetry delivery, encryption or app integration. JNI/Java and
Prefab/APK packaging remain separate work.

Local checks: `python3 android-native/test_verify.py`, Python syntax and C11 syntax
against the public headers. Existing local environment has no CMake/NDK; native
cross-compilation, installed import resolution and real ELF inspection are unrun
until the exact candidate's authorized hosted workflow succeeds. Scripts capture
bounded tool output and emit only fixed stages, never raw logs or arbitrary paths.

Official references:
- https://developer.android.com/ndk/guides/cmake
- https://developer.android.com/ndk/guides/stable_apis
- https://developer.android.com/guide/practices/page-sizes

CI run 44489885004 passed tool invocation but stopped before source archive. The
previous version check required an immediate LF after `3.22.1`, so valid CRLF or
CMake git-suffixed version strings were rejected. The parser now requires the exact
3.22.1 tuple while accepting a hexadecimal `-g` suffix (optionally `-dirty`) and
normalizing line endings. This is a source-proven parser defect, not confirmation
of the suppressed CI output or sole cause. CMake's own version construction:
https://github.com/Kitware/CMake/blob/v3.22.1/Source/CMakeVersion.cmake

Separate fixed preflight stages identify version parsing, sysroot directory,
target zlib library, zlib header, and each workspace/producer directory creation.
Target zlib paths and tool versions are unchanged. Missing sysroot/library/header
and directory errors produce distinct fixed labels, never raw paths or tool text.
Synthetic tests cover each case; actual NDK cross-build remains pending.

Run 44491699183 passed producer build/install and failed independent consumer
configuration. The consumer now receives exact MadObs_DIR at the moved package's
lib/cmake/MadObs, rather than relying on CMAKE_PREFIX_PATH discovery through the
Android cross-compilation root policy. Config, exact-version config and both
export files must exist before configuration. The existing exact 0.1.0 package
request and target-sysroot ZLIB dependency remain unchanged; global root lookup
rules are not relaxed. This addresses a plausible discovery issue, not a claim
that the suppressed CI error confirmed its cause. Configure failures now emit
exactly one fixed category: package, dependency, try-compile, or other (including
ambiguous/unrecognized errors), based on a bounded captured log tail. No raw
message, path, environment or payload is emitted.

MadObs lookup is restricted to the explicit moved config directory with
NO_DEFAULT_PATH/NO_CMAKE_FIND_ROOT_PATH, only for that package lookup. The requested
config must exist and the canonical resolved MadObs_CONFIG must equal the requested
canonical config, or configuration fails with a fixed exact-config label. Normal
NDK dependency lookup remains unchanged. Local source/label tests do not substitute
for an actual CMake ambient-package negative test; CMake is unavailable locally.
No hosted consumer success is claimed yet.
