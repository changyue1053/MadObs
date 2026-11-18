# Same-host bidirectional compiler package gate

`scripts/verify-cross-compiler-package.sh` requires existing GCC/G++ and Clang/
Clang++, records their resolved paths and versions, and fails if any is missing.
It neither installs toolchains nor accepts new SDK agreements. Producer and
consumer compiler paths are explicitly passed to fresh CMake configurations.

The two directions are GCC-built runtime -> Clang-built consumers and Clang-built
runtime -> GCC-built consumers. Each independently builds/installs a Release
null-provider static package, copies its installation prefix, then builds, links
and actually executes private C, strict C++11 private, raw runtime C and API-only C
consumers with the other compiler. Archive hashes, actual link commands and ldd
output are logged. API-only is a header check; the other three consume the archive.
No compile-only success is reported as executed consumption.

The gate runs on the GitHub Linux runner because the local workspace lacks Clang.
A successful actual job is required before claiming a pass. Runner/compiler
versions are recorded, not frozen. This qualifies only the observed same-host,
default standard-library configuration, not libc++, Windows, Apple/Android/OHOS,
BoringSSL/JNI combinations, arbitrary compiler versions, a frozen public ABI or
source/build provenance. Existing independent native/provider/JVM tests remain.

The script intentionally retains its printed `mktemp` evidence directory for
inspection. On persistent hosts, the operator is responsible for removing that
specific directory after collecting evidence; it must not be confused with other
projects' temporary files. Original source/build/install paths remain visible, so
copying the prefix is not a filesystem-isolated relocation proof.
