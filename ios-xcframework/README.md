# Internal iOS static XCFramework validation

Based solely on public MadObs 4af77ffa464a48307e5ff2d95f10b835919d205b,
source tree 3a03801beb18c9601630cc4421b95c2743018dfb. This is internal source-driven
packaging validation, not permission to distribute MadObs or a new license grant.
No binaries, generated artifacts, upstream copies or license changes are committed
or uploaded. Existing core, Java/JNI, Android and default iOS workflows are unchanged.

A separately scoped exact-SHA workflow uses existing macos-15/Xcode26.2 tools and
SDK26.2. No installation, account, license-acceptance or hardware-device access.
Both ARM64 device and simulator archives use the existing CMake install target,
null crypto and explicit target SDK zlib. Apple xcodebuild -create-xcframework
packages these with -library/-headers; no hand-crafted framework plist or dynamic
framework embedding. Public C ABI headers are runtime.h, observer.h, privacy.h and
the compatibility C types header imobs_c.h. No private C++ headers are exported.
A module map is supplied, but this checkpoint tests direct C header consumption,
not Swift or Clang module-import compatibility.

The generated XCFramework is moved to a spaced path, and its complete disposable
producer source/build/install directories are deleted. Two plist-validated slices
must be static archives for ARM64 ios/device and ios/simulator with identical public
headers. The independent Xcode consumer selects paths from that validated relocated
artifact and explicitly links its selected archive; it never uses a staged install,
producer header, ambient package, or custom PBX source-tree. This validates explicit
artifact consumption, not automatic Xcode XCFramework selection or distribution.
Target system zlib/libc++ and Apple frameworks remain explicit link inputs.

Existing App/main.m, Tests/APIOnly.c and Tests/NativeTests.mm are copied byte-for-byte.
No existing policy assertions, counters, sink callbacks or ownership/lifecycle
behavior is rewritten. A separate compilation prefix requires ARM64. Device
build-for-testing must link first, but no device binary is run. Then one created
UUID-bound iOS26.2 simulator runs the exact existing two-case suite; the unchanged
result verifier rejects wrong names, failures, skips, missing or extra tests.
Only this job's simulator is deleted. Output is fixed stages, not raw logs or payloads.
All temporary packages and consumer outputs are deleted on normal completion.

Local Linux evidence: synthetic plist/path/header rejection, unchanged fixture
bytes, independent generated project, fixed-output failures, existing exact-result
parser tests and Python compilation. CMake/Xcode are absent locally, so real Apple
archive packaging, device link, relocated simulator link and XCTest execution are
unrun until this exact candidate's authorized hosted gate passes. No stability,
production, persistence, encryption or remote-delivery claim is added.

Apple packaging reference:
https://developer.apple.com/documentation/xcode/creating-a-multi-platform-binary-framework-bundle

## Opt-in iOS two-runtime isolation acceptance

The `checkpoint/madobs-ios-runtime-isolation` branch runs the same relocated
binary-only consumer and requires one additional exact, named XCTest. It opens
two concurrently live runtime handles in separate private test directories
inside **one app sandbox**, each with a disjoint fixed event-ID policy. The
sample sanitizes its only event before admission and validates each encoded
batch against the correct policy in a bounded local discard callback. A is
stopped and destroyed before a new B admission; exact totals A=1 and B=2,
zero queue/WAL drops, and zero invalid sink batches are mandatory. Any
shutdown failure leaves its callback state and directory untouched until
simulator teardown. No network, payload retention, or device run.

This is independent runtime and graceful-shutdown isolation, not delivery,
WAL replay, off-device export, inter-app, crash recovery, or release evidence.
The public packaged header documents terminal stop/new-instance semantics,
not a guaranteed replay-on-reopen contract for rejected batches. The default
preview still requires its original two tests. No package layout, production
code, public ABI, privacy declaration, project link settings or SDK pin changed.
Portable tests exercise generated source and exact result gating. Hosted Apple
build and simulator execution are required to substantiate the new behavior.
