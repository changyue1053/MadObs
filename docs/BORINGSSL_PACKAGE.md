# Optional verified-source provider package

Default remains no BoringSSL, with encryption unavailable rather than emulated. Any
explicit but incomplete BoringSSL input now fails configure rather than silently
falling back. In-tree use can supply an external provider; installation requires
`MADOBS_PACKAGE_BORINGSSL=ON` and `MADOBS_BORINGSSL_SOURCE_ARCHIVE`.

The pinned official source identity is in deps/boringssl.lock.json. The offline source
verifier checks the archive checksum, exact source file contents (8,657 files at this
pin), absence of extra files and links, and safe paths. Build out of tree. This source
pin is an engineering baseline, not a claim it is the latest/security-approved version.

```
cmake -S . -B build-provider -DCMAKE_BUILD_TYPE=Release \
  -DIMOBS_BORINGSSL_INCLUDE=/private/boringssl/include \
  -DIMOBS_BORINGSSL_CRYPTO_LIB=/private/provider-build/libcrypto.a \
  -DMADOBS_PACKAGE_BORINGSSL=ON \
  -DMADOBS_BORINGSSL_SOURCE_ARCHIVE=/private/source.tar.gz
```

The provider must be built with PIC for shared MadObs builds. Installed static SDK
packages contain lib/madobs/libmadobs_boringssl_crypto.a, upstream BoringSSL LICENSE,
Fiat LICENSE/AUTHORS, pinned source metadata and binary identity evidence. Package
config imports a prefix-relative MadObs::BoringSSL target, checks its archive SHA256,
and rejects a conflicting preexisting target. No source/build absolute archive path
is exported. Missing/tampered bundled archives fail configuration. No headers from
another SDK are needed by the installed consumer.

The source verifier does NOT prove that an externally supplied libcrypto archive was
compiled from that source. Binary SHA256 checks packaged bytes, not compiler provenance.
This is artifact/source integrity evidence plus observed local build/test results,
not a signed/reproducible-build attestation, full transitive SBOM, vulnerability audit,
FIPS validation, or first-party licensing decision. Compiler/system-zlib/toolchain
and upstream compatibility still require maintenance. Third-party license files are
copied unchanged with bundled binary distributions; no first-party license is invented.

## Symbols and consumer composition

Shared runtime export audits hide provider symbols and permit only MadObs/legacy C ABI.
Static libcrypto retains upstream symbol names. Linking another OpenSSL/BoringSSL
version (including via another SDK) can cause symbol/version conflicts or accidental
resolution. This work does NOT certify arbitrary cross-SDK static composition. Hosts
must coordinate a compatible single provider plan and validate their final link; do
not treat differently-named archive files as isolated symbol namespaces. The installed
MadObs::BoringSSL target check catches that target's collisions only, not every possible
third-party provider or platform loader interaction.

## Verification limits

Local bwrap validation hides original source/build/workspace paths and successfully
builds/runs a relocated encrypted-log C consumer using only installed files and system
toolchains. GitHub CI separately builds the real pinned provider and tests a copied
prefix; its copy test alone is not a sandbox-isolation proof. Mobile SDKs and devices
remain unverified. See docs/PHASE8_RESULT.md for exact final evidence.

Reproduce local isolation with `scripts/verify-provider-isolation.sh <installed-prefix>`
and an actual native CMAKE executable. Set MADOBS_PROVIDER_SOURCE/BUILD to require
those original paths to be absent in the test namespace; set TMPDIR to an owned build
workspace. The script requires Linux bwrap and fails rather than skipping isolation.
