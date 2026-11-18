# Repeatable, fixed MadLink / MadObs host fixture

This optional test does not add MadLink to default MadObs dependencies. It consumes
caller-supplied, authorized artifacts matching `deps/madlink-host-composition.lock.json`.
It does not download SDKs, accept licenses or publish the private fixture it creates.
No third-party headers or archives are added to this source repository.

```sh
export CMAKE=/path/to/cmake/bin/cmake
export MADOBS_COMPOSITION_PACKAGE=/path/to/fixed/MadObs-install
export MADLINK_COMPOSITION_SOURCE=/path/to/fixed/MadLink-source
export MADLINK_COMPOSITION_ARCHIVE=/path/to/fixed/libimlink.a
export MADLINK_COMPOSITION_CORE_ARCHIVE=/path/to/fixed/libimlink_core.a
export MADLINK_COMPOSITION_SSL_ARCHIVE=/path/to/matching/libssl.a
export MADOBS_COMPOSITION_WORK_ROOT=/your/own/workspace-with-space
scripts/verify-madlink-composition.sh
```

Linux, bwrap, Python with `O_NOFOLLOW`, GCC/G++ and CMake are required. The workspace
parent must already exist and be host-controlled. Use a CMake executable with its
complete installation prefix, not a Python wrapper referencing hidden paths.
A failed attempt leaves its private partial fixture for inspection; it is never
compiled after verification fails. Use a new generated work directory for retries.

## What is checked

The preparer rejects unexpected/missing package and selected-header files,
symlink entries/final file symlinks, nonregular or >256MiB individual inputs, unsafe manifest paths, bad
hashes and existing output files. It reads each file once, hashes those bounded
bytes, then writes exactly those bytes to a fresh private bundle. It does not
verify and later build from a mutable source pathname. Archive/source changes
require deliberate revalidation and a new lock; silently accepting new hashes
would defeat the guard.

The bundle holds all 21 installed MadObs files, nine selected MadLink public
headers, three static archives and the three-file smoke sample. bwrap exposes that
bundle at new paths while the original SDK/source/build paths are unavailable.
Only the sample is built; it instantiates/releases MadLink and exercises the
MadObs adapter/private-runtime lifecycle without calling network Start. It runs
20 times. A single matching crypto archive is linked; whole-archive is not used.

## Limits

This is byte-integrity and one host-composition fixture, not signed build
provenance, a bit-reproducible build claim, a general cross-SDK ABI guarantee or
permission to redistribute supplied dependencies. The recorded local MadLink
commit is an older frozen baseline, not current public main. System compiler,
libc/libstdc++ and zlib come from the host; their transitive binary bytes are not
locked. Compiler version and the final link command are logged. This is not a
fully hermetic toolchain or a security sandbox against malicious concurrent host
filesystem changes. Keep input/output parent directories host-private.

Filesystem-path isolation is tested. Network namespace creation was denied by this
execution environment, so network isolation is NOT claimed. The sample does not
start network traffic, but the script inherits the host network namespace. No new
Android NDK agreement/toolchain or mobile validation is involved. See
`PHASE15_RESULT.md` for the exact observed scope and preserved failure evidence.

Fast synthetic guard tests require no dependency binaries and run in native CI:

```sh
python3 scripts/composition/test_prepare.py
```

The complete fixed-artifact composition remains opt-in, not silently skipped or
reported as executed by that synthetic CI step.
