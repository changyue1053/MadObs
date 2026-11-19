# Local iOS build-validation candidate, not a distributable SDK

Input: the public MadObs commit `79fd8ced44a18bca574598ff683cd706ded16a60`,
source tree `fa87aed52ec6d748addacb2f8d57ac30e5976271`.

This directory adds an **in-repository experimental validation recipe only**.
It does not publish, distribute, release, upload, sign for distribution, or grant
rights to the existing code. The repository currently has **no source LICENSE**.
Do not infer MIT, Apache, or any other license from this work or from another Mad SDK.
Resolve the existing source licensing/attribution separately before distribution.
No XCFramework or downloadable package is added. A separate checkpoint-only
verification workflow is proposed below; it is not a distribution workflow.

## Two separate kinds of evidence

1. `bash scripts/verify-ios-api-only.sh` copies only `observer.h` and `privacy.h`
   plus a C consumer into a fresh temporary directory. It compiles and executes
   with the existing host C compiler, without linking a runtime, zlib, disk sink,
   threads, or network code. This is genuinely API-only consumption, **not iOS
   execution**. Synthetic numeric fields exercise Noop, callback copying, an
   explicit finite-domain allowlist and removal of disallowed attributes/timing.
2. `bash scripts/build-ios-validation.sh` is a **pending Apple build** recipe for
   the full runtime and an independent application-hosted XCTest consumer. It
   creates separate device/simulator ARM64 static builds, stages their headers
   and archive inside `build/ios-validation`, and invokes Xcode build-for-testing.
   The consumer references only these staged headers and the compiled archive,
   never producer sources or another Mad SDK. The runtime resides in the XCTest
   bundle, which loads into the real validation application process when tests run.
   Build-for-testing does not launch that process and is not runtime evidence.

## Exact intended Apple environment

Already-installed/licensed Apple-silicon macOS, Xcode **26.2 (17C52)** at
`/Applications/Xcode_26.2.app`, device/simulator SDK **26.2**, and an existing
CMake >=3.20 with its Xcode generator. iOS **15.0** is a proposed deployment target,
not a minimum-version runtime certification. Only ARM64 is built. These scripts
never download tools, SDKs or runtimes, accept agreements, connect physical devices,
or invoke a cloud executor. `build/ios-validation` must not already exist; the build
refuses to delete or mix prior artifacts. No local Apple build has been performed.

## Runtime configuration and dependencies

The public runtime implementation and its source list are unchanged. C++20 and
**the selected Apple SDK's zlib header and libz.tbd** are retained. The two builds
explicitly turn shared libraries, desktop tests/tools, JNI, sanitizers and BoringSSL
packaging off, and clear all provider input variables in fresh build directories.
The resulting runtime is the existing **null-provider** implementation. This means
no encryption qualification, not a substitute cryptographic provider.

The full source list still compiles server, TCP and crash modules. This stage does
not remove them or claim a network-free binary. A static linker may omit unreferenced
objects; that is not an audited feature-removal guarantee. No network, raw logging,
control key, crash handler or healing API is invoked by the new consumer. Running
existing desktop/server/crash suites is deliberately not part of the iOS recipe.

The consumer uses the existing **C ABI** (with an Objective-C++ test host). It sends
only constant synthetic numeric events into a host-provided local sink that counts
callbacks and retains no payload. Each callback completes exactly once and never
calls runtime lifecycle operations. Successful completion means local consumption,
not remote delivery. The sole producer stops before stop/destroy. Context storage
outlives drain/destruction. Tests cover dispatch versus admission, final generic
counts, terminal stop, destroy and stale-handle rejection. No callback-lifetime,
concurrent-destroy, performance, WAL recovery, or delivery guarantee is inferred.

## Optional real Simulator execution, separately authorized

Only after successful Apple builds, on the prepared authorized Mac:

```
MADOBS_RUN_SIMULATOR=1 bash scripts/test-ios-validation-simulator.sh
```

This requires a preinstalled iOS26.2 runtime. It creates its own iPhone16 Simulator,
checks ARM64, and uses only that UUID. It ad-hoc signs and installs the validation
app/test bundle into that simulator, runs XCTest and then best-effort shuts down
and deletes that simulator. It does not select/delete preexisting simulators or
physical devices. INT/TERM trigger exit cleanup; forced termination cannot promise
cleanup. The explicit opt-in is not permission to run on somebody else's machine.
All build output and the `.xcresult` remain in the executor's ignored local
build directory. No artifact/log upload is provided.

## Proposed checkpoint verification workflow

`.github/workflows/ios-validation.yml` runs only on a push to
`checkpoint/madobs-ios-validation`, with a second exact-ref job guard. It does not
run on main, tags or PRs, has read-only contents permission, and does not retain
checkout credentials. Checkout is pinned to the public v7.0.1 commit.

On GitHub's ARM64 `macos-15` runner, it first runs the independent API-only check,
then requires preinstalled Xcode26.2/17C52, both SDK26.2 variants, their zlib inputs,
CMake and the iOS26.2 Simulator runtime. Missing prerequisites fail, rather than
installing tools, accepting licenses, changing platforms or reporting a skipped
runtime test as success. The hosted image is rolling; only the Apple versions are
exactly checked. No new external package dependency is introduced.

The workflow builds both actual archives and consumer test bundles, then explicitly
opts into the job-owned Simulator test. Device build success is not device execution.
A successful xcodebuild exit alone is not enough: targeted xcresulttool summary/tests
JSON must report exactly two passed cases, zero failed/skipped cases, and exactly
`MadObsNativeTests/testHeaderOnlyContract` plus
`MadObsNativeTests/testInstalledArchiveLocalSinkAndLifecycle`, both Passed. Missing,
duplicate, extra, skipped or failed cases fail closed. Nested failed attempts are
also rejected. JSON is processed in memory, not exported or printed. Unknown result
schema fails rather than silently dropping the gate.
Apple build/simulator command output stays in ignored executor-local logs. The
workflow emits only fixed stage labels and the two fixed test names/counts; it does
not dump raw result JSON, checkout file diffs, device output, event payloads,
credentials or environment. The checkout action may emit its standard operational
metadata; no repository log/file dump is added. No artifacts are uploaded. The job has no external
exporter endpoint and makes no network delivery or encryption claim. No workflow
has been submitted or executed by preparing this local candidate. Its eventual
publication and first run require the separate approval/review process.

## Verification status

Executed locally on Linux: independent copied-header C11 API-only consumer passed;
shell syntax, project-generator consistency, Python/XML parsing and publication
scope/whitespace checks passed. Eight synthetic result-parser regressions passed;
these are not XCTest execution evidence. The result parser has not yet consumed a
real Xcode26.2 bundle in this candidate. This is not an SDK delivery or release.

Not executed: CMake Apple cross-build, target-zlib resolution, Apple C++20/SDK
portability, Objective-C++/XCTest compilation, static-archive linking, application
installation, Simulator test execution, minimum-iOS or physical-device behavior.
There is no fake runtime or mocked Apple framework in these checks. Existing Linux
runtime evidence does not establish iOS compatibility. A failure in the full runtime
build is reported as such; API-only success never substitutes for it.

## Primary references

- Public source: https://github.com/changyue1053/MadObs/tree/79fd8ced44a18bca574598ff683cd706ded16a60
- Existing contracts: [PUBLIC_API.md](../PUBLIC_API.md) and [platforms/README.md](../platforms/README.md).
- Apple build-for-testing: https://developer.apple.com/library/archive/technotes/tn2339/_index.html
- CMake iOS toolchains: https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling-for-ios-tvos-or-watchos

## Diagnostic follow-up

The first hosted checkpoint (public commit `1badb77df670549ded985b4004bd831386cad334`,
run `44462179943`) passed API-only verification, prerequisites and device/simulator
build-for-testing. The Simulator step failed; its former combined diagnostic does
not establish whether setup, execution or result parsing failed. No runtime success
is claimed. This follow-up emits only fixed PASS/FAIL stages: sim-prerequisites,
sim-create, sim-boot, sim-arm64, xctest-execute and named-result-parse. Parser
rejection reasons distinguish missing count/status schema, incorrect counts/status,
identifier mismatch, missing cases, query command failure/timeout and invalid JSON.
No raw tool output, identifiers from results, paths, JSON or device logs are printed.

The execution action deliberately remains `test`, with the existing ad-hoc signing
flags. The earlier generic build-for-testing disabled signing, so changing directly
to test-without-building would be an additional unverified installability change.
There are no retries or altered native assertions in this diagnostic follow-up.

Apple's [Xcode 16.3 release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-16_3-release-notes)
confirm the `get test-results summary` and `tests` commands. The reviewed official
web documentation does not establish exact Xcode 26.2 JSON field/value schemas.
This follow-up does not guess new schema aliases or relax named-case acceptance;
it distinguishes schema failures so subsequent changes require measured evidence.
Thirteen synthetic parser tests run locally; these are not real XCTest evidence.
