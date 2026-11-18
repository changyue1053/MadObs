# Clang cross-compiler CI gate

This additional Linux CI job checks that the runner really provides `clang` and
`clang++`, records both resolved paths/version strings, and configures fresh native
Release and API-only builds with explicit CC/CXX. Missing compiler, warnings treated
as errors, compilation failure, test failure or installed-consumer failure makes
the job fail. It does not silently fall back to GCC or use continue-on-error.

The job also runs the existing installed/relocated native C/C++11/API consumer
checks under the same compiler environment. Its tested configuration is default
null provider. This does not qualify Clang plus BoringSSL/JNI, libc++ (the runner
may select libstdc++), Apple Clang/Xcode, Android or another target ABI.

No Clang compiler is present in the current local workspace. This gate therefore
must be evaluated on its actual GitHub runner before claiming a Clang pass. The
runner image/compiler versions are not frozen; version output in each job records
what was actually used. This change neither installs a new local toolchain nor
accepts a new SDK agreement. Existing GCC/provider/host-JVM jobs remain independent.
