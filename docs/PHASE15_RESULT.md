# Fifteenth increment: repeatable fixed cross-SDK fixture

Status: one fixed Linux x86_64 GCC 14.2.0 / CMake 4.4.5 / zlib 1.3.1 fixture
passes after byte-locked copying and source-path isolation. No production SDK
implementation changed. Setup and limits: FIXED_COMPOSITION.md.

## Inputs and mutable-artifact discovery

The previous Phase9 `build-production` paths had changed: observed MadLink archive
SHA256 prefixes 3df815ae / bec2b2ef no longer matched the frozen 5c0d2535 / b7912369
artifacts. They were not combined with the old headers. No MadLink source/build
file was edited or deleted.

Instead, local MadLink commit 53668ab8a06ac09d07dcadefccbcf106b35dff79, tree
52f55936d94c948e96b9defc1d688f790d02bb0c, was exported read-only into a MadObs-owned
workspace and built with Release, BUILD_TESTING=OFF, IMLINK_BUILD_TESTS=OFF,
MADLINK_ENABLE_QUIC=OFF and explicit same-pin BoringSSL source/build inputs.
This single rebuild yielded the same two archive hashes observed in Phase9.
That observation is NOT a cross-machine/reproducible-build or signed-origin proof.

At inspection, MadLink local HEAD was 3bfafe0918686b0539bede6b5f8eddf5c6db9470 and
public main was bfb397dbc0558a971626019a85bcc7bbe41d5121. Neither is the old local
fixture commit above. This stage does not qualify current MadLink main's behavior.
The source commit/tree and archive hashes describe distinct facts; a hash alone
does not prove which source/toolchain produced an archive.

MadObs package checkpoint: local 216dc94e07d9f3e6bbd967d73ab378542165f7bf equals
public 5ddbbe9e3a6f1a4adc0f6ccf9a25635f9477df48 by full tree. The fresh installed
prefix contains runtime SHA256 bea81b6d5ecd4eaffbd91bb054c1ff8f9ebbe6d5f91540170760064fb1f0a358
and PIC crypto SHA256 8a715430ae5ca6f471206132ebbc69f54d42d7f495ec05beb6e028ca875a78c0.
BoringSSL upstream pin remains 533902fb2e8fd0586db7d58879d697721726605a.
Full copied file/hash inventory is deps/madlink-host-composition.lock.json; no
archive, dependency source or generated fixture binary is committed here.

## Tests

- Exact package/header/archive/sample bytes are checked before copying and build.
  Original source/build/input paths are explicitly absent in the isolated build.
- Strict sample compilation succeeds; one composition CTest passes 20 consecutive
  runs. Link line records one crypto provider, not whole-archive.
- Ten synthetic guard tests cover snapshot independence, wrong hash, extra/missing
  files, file/directory symlinks, unsafe paths, overwrite refusal, oversize input
  and nonblocking FIFO rejection. This fast guard suite is added to native CI;
  it is not the full composition test.
- An actually corrupted copy of the fixed archive rejects before invoking build.
- A network-namespace attempt failed with NETLINK_ROUTE permission denial. It was
  not elevated or bypassed. The final script claims filesystem-path isolation
  only and the no-Start sample behavior, not network isolation.

Evidence: verification/phase15-composition-isolated.log,
phase15-integrity-tests.log, phase15-corrupt-input.log and
phase15-network-namespace-unavailable.log.

## Prior Clang gate result (Phase14)

Public checkpoint 5ddbbe9e3a6f1a4adc0f6ccf9a25635f9477df48 passed all five jobs in
https://github.com/changyue1053/MadObs/actions/runs/44291666338 . The actual compiler
was Ubuntu Clang 18.1.3 at /usr/bin/clang{,++}: native 43/43, API-only 6/6 and
installed/relocated C/C++11 consumers passed. Selected version/result lines are
preserved in verification/phase14-clang-ci.log. This does not claim Clang with
BoringSSL/JNI, libc++, Apple Clang or a mobile platform has passed.
